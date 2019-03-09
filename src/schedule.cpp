#include <migraphx/schedule.hpp>
#include <migraphx/program.hpp>
#include <migraphx/instruction.hpp>
#include <migraphx/operators.hpp>
#include <migraphx/iterator_for.hpp>
#include <migraphx/dfor.hpp>
#include <migraphx/functional.hpp>
#include <migraphx/ranges.hpp>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <deque>

namespace migraphx {
inline namespace MIGRAPHX_INLINE_NS {

auto get_inputs()
{
    return [](auto i) { return i->inputs(); };
}

auto get_outputs()
{
    return [](auto i) { return i->outputs(); };
}

struct stream_info
{
    std::unordered_map<instruction_ref, std::size_t> ins2stream;
    std::unordered_map<instruction_ref, std::size_t> weights;
    std::unordered_map<instruction_ref, std::size_t> iweights;

    void accumulate_weights(instruction_ref last, const schedule_model& model)
    {
        fix<std::size_t>([&](auto self, auto ins) -> std::size_t {
            if(not contains(weights, ins))
            {
                std::size_t weight = 0;
                auto&& op          = ins->get_operator();
                if(not is_context_free(op) and op.name()[0] != '@')
                    weight = model.weight(op);
                iweights[ins] = weight;
                weights[ins] =
                    std::accumulate(ins->inputs().begin(),
                                    ins->inputs().end(),
                                    weight,
                                    [&](std::size_t w, instruction_ref i) { return w + self(i); });
            }
            return weights[ins];
        })(last);
    }

    struct partition
    {
        std::size_t weight = 0;
        std::vector<instruction_ref> instructions{};

        void add(instruction_ref ins, std::size_t w)
        {
            weight += w;
            instructions.push_back(ins);
        }
    };

    void assign_streams(program& p, std::size_t n)
    {
        const std::size_t min_partition_threshold = 2;
        partition critical;
        std::unordered_map<instruction_ref, std::deque<partition>> partitions;
        fix([&](auto self, auto ins, auto& part) {
            // If weight is zero then stop
            if(this->weights[ins] == 0)
                return;
            part.add(ins, this->iweights[ins]);

            auto max_it = std::max_element(ins->inputs().begin(),
                                           ins->inputs().end(),
                                           by(std::less<>{}, index_of(this->weights)));
            for(auto i : ins->inputs())
            {
                const auto weight = this->weights[i];
                if(i == *max_it or weight <= min_partition_threshold)
                {
                    self(i, part);
                }
                else
                {
                    partitions[ins].emplace_back();
                    self(i, partitions[ins].back());
                }
            }
        })(std::prev(p.end()), critical);

        // Set the critical partition to stream 0
        set_stream(critical, 0);
        std::vector<std::size_t> streams(n - 1);
        // Assign streams for the other partitions
        for(auto&& ins_part : partitions)
        {
            std::sort(
                ins_part.second.begin(), ins_part.second.end(), by(std::greater<>{}, [](auto&& x) {
                    return std::make_tuple(x.weight, x.instructions.size());
                }));
            for(auto&& part : ins_part.second)
            {
                auto stream = std::min_element(streams.begin(), streams.end()) - streams.begin();
                set_stream(part, stream + 1);
                streams[stream] += part.weight;
            }
        }
    }

    void set_stream(const partition& p, std::size_t n)
    {
        for(auto ins : p.instructions)
            if(iweights[ins] > 0)
                set_stream(ins, n);
    }

    void set_stream(instruction_ref ins, std::size_t n)
    {
        assert(iweights[ins] > 0);
        ins2stream[ins] = n;
    }

    std::size_t get_stream(instruction_ref ins) const { return ins2stream.at(ins); }

    bool has_stream(instruction_ref ins) const { return contains(ins2stream, ins); }

    bool different(const std::vector<std::size_t>& v) const
    {
        if(v.size() < 2)
            return false;
        return not std::all_of(v.begin(), v.end(), [&](std::size_t x) { return x == v.front(); });
    }

    template <class F>
    bool different(F f, std::size_t stream) const
    {
        bool result = false;
        f([&](auto s) {
            if(s != stream)
            {
                result = true;
                return false;
            }
            stream = s;
            return true;
        });
        return result;
    }

    template <class F>
    bool different(F f) const
    {
        bool result = false;
        f([&](auto s) {
            result = different(f, s);
            return false;
        });
        return result;
    }

    template <class Selector>
    auto get_streams_from(instruction_ref start, Selector select) const
    {
        return [=](auto f) {
            return fix<bool>([&](auto self, auto ins) {
                for(auto i : select(ins))
                {
                    if(iweights.at(i) == 0)
                    {
                        if(not self(i))
                            return false;
                    }
                    else
                    {
                        if(not f(get_stream(i)))
                            return false;
                    }
                }
                return true;
            })(start);
        };
    }

    std::unordered_set<std::size_t> get_streams(instruction_ref ins)
    {
        if(has_stream(ins))
            return {get_stream(ins)};
        std::unordered_set<std::size_t> result;
        get_streams_from(ins, get_inputs())([&](auto s) {
            result.insert(s);
            return true;
        });
        return result;
    }

    template <class... Ts>
    bool is_merge_point(instruction_ref ins, Ts... xs) const
    {
        return different(get_streams_from(ins, get_inputs()), xs...);
    }

    template <class... Ts>
    bool is_split_point(instruction_ref ins, Ts... xs) const
    {
        return different(get_streams_from(ins, get_outputs()), xs...);
    }

    std::vector<instruction_ref> get_recorded_instructions(instruction_ref start)
    {
        std::vector<instruction_ref> result;
        std::unordered_map<std::size_t, instruction_ref> m;
        fix([&](auto self, auto ins) {
            for(auto i : ins->inputs())
            {
                if(iweights.at(i) == 0)
                {
                    self(i);
                    continue;
                }
                auto stream = get_stream(i);
                if(not contains(m, stream))
                    m[stream] = i;
                else
                    m[stream] = std::min(m[stream], i, by(std::less<>{}, [&](auto x) {
                                             return std::distance(x, start);
                                         }));
            }
        })(start);
        std::transform(
            m.begin(), m.end(), std::back_inserter(result), [](auto&& p) { return p.second; });
        return result;
    }

    std::vector<std::size_t> wait_for(instruction_ref ins) const
    {
        std::vector<std::size_t> result;
        get_streams_from(ins, get_inputs())([&](auto s) {
            result.push_back(s);
            return true;
        });
        // Remove duplicates
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        // Remove the merged stream
        auto it = std::find(result.begin(), result.end(), get_stream(ins));
        if(it != result.end())
            result.erase(it);
        return result;
    }

    std::unordered_map<instruction_ref, std::vector<std::vector<instruction_ref>>>
    find_concurrent_instructions(program& p)
    {
        std::unordered_map<instruction_ref, std::vector<std::vector<instruction_ref>>> result;
        std::unordered_map<instruction_ref, std::unordered_set<instruction_ref>> merge_from;
        for(auto ins : reverse_iterator_for(p))
        {
            for(auto&& arg : ins->outputs())
            {
                if(is_merge_point(arg))
                    merge_from[ins].insert(arg);
                merge_from[ins].insert(merge_from[arg].begin(), merge_from[arg].end());
            }

            auto streams = get_streams(ins);

            // Collect concur instructions for each merge point.
            for(auto& merge : merge_from[ins])
            {
                for(auto stream : streams)
                {
                    if(result[merge].size() <= stream)
                        result[merge].resize(stream + 1);
                    auto&& r = result[merge][stream];
                    r.push_back(ins);
                    // Copy inputs if they dont have a stream(and are not a builtin and context free)
                    // Inputs without a stream can have a implicit dependency
                    std::copy_if(ins->inputs().begin(), ins->inputs().end(), std::back_inserter(r), [&](auto x) {
                        return not this->has_stream(x) and not is_context_free(x->get_operator()) and x->name().front() != '@';
                    });
                }
            }
        }
        return result;
    }
};

void schedule::apply(program& p) const
{
    stream_info si;
    auto last = std::prev(p.end());
    si.accumulate_weights(last, model);
    si.assign_streams(p, model.concurrency());

    // Topo sort
    fix([&](auto self, auto ins) {
        auto args = ins->inputs();
        std::sort(args.begin(), args.end(), by(std::less<>{}, [&](auto x) {
                      return std::make_tuple(si.weights[x], x->inputs().size());
                  }));
        for(auto i : args)
        {
            p.move_instruction(i, p.begin());
            self(i);
        }
    })(last);

    if(enabled(MIGRAPHX_TRACE_COMPILE{}))
    {
        p.annotate(std::cout, [&](auto ins) {
            std::cout << ":";
            std::cout << " weight=" << si.weights.at(ins);
            std::cout << " input={";
            si.get_streams_from(ins, get_inputs())([&](auto s) {
                std::cout << s << ",";
                return true;
            });
            std::cout << "}";
            if(si.has_stream(ins))
                std::cout << " stream=" << si.get_stream(ins);
        });
        std::cout << std::endl;
    }

    // Schedule instructions
    std::unordered_map<instruction_ref, std::size_t> ins2wait;
    std::size_t wait_id = 0;
    std::unordered_map<std::size_t, std::unordered_set<std::size_t>> waited_for;
    std::unordered_map<instruction_ref, std::unordered_set<std::size_t>> ins2waited;
    for(auto ins : iterator_for(p))
    {
        // Only schedule instructions that have a stream
        if(not si.has_stream(ins))
            continue;
        assert(si.weights[ins] > 0);
        // Schedule instruction on the stream
        auto stream = si.get_stream(ins);
        assert(stream < model.concurrency());
        model.sched(p, ins, stream);
        // Insert wait instructions
        if(si.is_merge_point(ins, stream))
        {
            for(auto i : si.get_recorded_instructions(ins))
            {
                if(not si.has_stream(i))
                    continue;
                auto istream = si.get_stream(i);
                if(stream == istream)
                    continue;
                // Create a new event if it hasn't been recorded
                if(not contains(ins2wait, i))
                {
                    ins2wait[i] = wait_id;
                    model.record(p, i, wait_id);
                    wait_id++;
                }
                auto w = ins2wait.at(i);
                // If we already waited for the event on this stream then dont
                // insert another wait event
                if(not contains(waited_for[stream], w))
                    model.wait(p, ins, w);
                // Store the event as waited
                waited_for[stream].insert(w);
                // Store all wait events that have been waited on prior to the recorded instruction
                waited_for[stream].insert(ins2waited[i].begin(), ins2waited[i].end());
            }
        }
        // Store wait events that have already been waited on
        if(si.is_split_point(ins, stream))
        {
            ins2waited[ins] = waited_for[stream];
        }
    }

    // Add memory conflicts
    auto concur_ins = si.find_concurrent_instructions(p);
    for(auto&& merge : concur_ins)
    {
        dfor(merge.second.size(), merge.second.size())([&](auto i, auto j) {
            if(i == j)
                return;
            for(auto ins1 : merge.second[i])
            {
                auto args = merge.second[j];
                args.insert(args.begin(), ins1);
                p.insert_instruction(merge.first, op::identity{}, args);
            }
        });
    }
}

} // namespace MIGRAPHX_INLINE_NS
} // namespace migraphx
