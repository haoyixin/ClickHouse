#pragma once

#include <Common/PODArray.h>
#include <Common/NaNUtils.h>
#include <Core/Types.h>
#include <IO/WriteBuffer.h>
#include <IO/ReadBuffer.h>
#include <IO/VarInt.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int BAD_ARGUMENTS;
}

/** Calculates quantile by collecting all values into array
  *  and applying n-th element (introselect) algorithm for the resulting array.
  *
  * It uses O(N) memory and it is very inefficient in case of high amount of identical values.
  * But it is very CPU efficient for not large datasets.
  */
template <typename Value>
struct QuantileExact
{
    /// Static interface for AggregateFunctionQuantile.
    using ValueType = Value;
    static constexpr bool has_second_arg = false;
    using FloatReturnType = void;
    static constexpr bool is_finalization_needed = true;

    /// The memory will be allocated to several elements at once, so that the state occupies 64 bytes.
    static constexpr size_t bytes_in_arena = 64 - sizeof(PODArray<Value>);
    using Array = PODArrayWithStackMemory<Value, bytes_in_arena>;
    Array array;

    void add(const Value & x)
    {
        /// We must skip NaNs as they are not compatible with comparison sorting.
        if (!isNaN(x))
            array.push_back(x);
    }

    template <typename Weight>
    void add(const Value &, const Weight &)
    {
        throw Exception("Method add with weight is not implemented for QuantileExact", ErrorCodes::NOT_IMPLEMENTED);
    }

    void merge(const QuantileExact & rhs)
    {
        array.insert(rhs.array.begin(), rhs.array.end());
    }

    void serialize(WriteBuffer & buf) const
    {
        size_t size = array.size();
        writeVarUInt(size, buf);
        buf.write(reinterpret_cast<const char *>(array.data()), size * sizeof(array[0]));
    }

    void deserialize(ReadBuffer & buf)
    {
        size_t size = 0;
        readVarUInt(size, buf);
        array.resize(size);
        buf.read(reinterpret_cast<char *>(array.data()), size * sizeof(array[0]));
    }

    size_t getElementNumber(Float64 level) const
    {
        return level < 1 ? level * array.size()
                         : (array.size() - 1);
    }

    void finalize(Float64 level)
    {
        if (!array.empty())
        {
            size_t n = getElementNumber(level);
            std::nth_element(array.begin(), array.begin() + n, array.end());    /// NOTE You can think of the radix-select algorithm.
        }
    }

    void finalize(const Float64 * levels, const size_t * indices, size_t size)
    {
        if (!array.empty())
        {
            size_t interval_start = 0;
            for (size_t i = 0; i < size; ++i)
            {
                auto level = levels[indices[i]];
                size_t n = getElementNumber(level);

                if (n + 1 == interval_start)
                    continue;

                std::nth_element(array.begin() + interval_start, array.begin() + n, array.end());
                interval_start = n + 1;
            }
        }
    }

    /// Get the value of the `level` quantile. The level must be between 0 and 1.
    Value get(Float64 level)
    {
        if (!array.empty())
            return array[getElementNumber(level)];

        return std::numeric_limits<Value>::quiet_NaN();
    }

    /// Get the `size` values of `levels` quantiles. Write `size` results starting with `result` address.
    /// indices - an array of index levels such that the corresponding elements will go in ascending order.
    void getMany(const Float64 * levels, const size_t * indices, size_t size, Value * result)
    {
        if (!array.empty())
        {
            for (size_t i = 0; i < size; ++i)
            {
                auto level = levels[indices[i]];
                result[indices[i]] = array[getElementNumber(level)];
            }
        }
        else
        {
            for (size_t i = 0; i < size; ++i)
                result[i] = Value();
        }
    }
};

/// QuantileExactExclusive is equivalent to Excel PERCENTILE.EXC, R-6, SAS-4, SciPy-(0,0)
template <typename Value>
struct QuantileExactExclusive : public QuantileExact<Value>
{
    using FloatReturnType = Float64;
    using QuantileExact<Value>::array;

    void finalize(Float64 level)
    {
        if (!array.empty())
        {
            if (level == 0. || level == 1.)
                throw Exception("QuantileExactExclusive cannot interpolate for the percentiles 1 and 0", ErrorCodes::BAD_ARGUMENTS);

            Float64 h = level * (array.size() + 1);
            auto n = static_cast<size_t>(h);

            if (n >= array.size())
                std::swap(*std::max_element(array.begin(), array.end()), array.back());
            else if (n < 1)
                std::swap(array.front(), *std::min_element(array.begin(), array.end()));
            else
            {
                std::nth_element(array.begin(), array.begin() + n - 1, array.end());
                std::swap(array[n], *std::min_element(array.begin() + n, array.end()));
            }
        }
    }

    /// Get the value of the `level` quantile. The level must be between 0 and 1 excluding bounds.
    Float64 getFloat(Float64 level)
    {
        if (!array.empty())
        {
            if (level == 0. || level == 1.)
                throw Exception("QuantileExactExclusive cannot interpolate for the percentiles 1 and 0", ErrorCodes::BAD_ARGUMENTS);

            Float64 h = level * (array.size() + 1);
            auto n = static_cast<size_t>(h);

            if (n >= array.size())
                return array.back();
            else if (n < 1)
                return array.front();

            return array[n - 1] + (h - n) * (array[n] - array[n - 1]);
        }

        return std::numeric_limits<Float64>::quiet_NaN();
    }

    void finalize(const Float64 * levels, const size_t * indices, size_t size)
    {
        if (!array.empty())
        {
            size_t interval_start = 0;
            for (size_t i = 0; i < size; ++i)
            {
                auto level = levels[indices[i]];
                if (level == 0. || level == 1.)
                    throw Exception("QuantileExactExclusive cannot interpolate for the percentiles 1 and 0", ErrorCodes::BAD_ARGUMENTS);

                Float64 h = level * (array.size() + 1);
                auto n = static_cast<size_t>(h);

                if (n >= array.size())
                    std::swap(*std::max_element(array.begin(), array.end()), array.back());
                else if (n < 1)
                    std::swap(array.front(), *std::min_element(array.begin(), array.end()));
                else
                {
                    /// we need to place correct elements on positions n - 1 and n
                    /// if elements on positions interval_start - 2 and interval_start - 1 are correct

                    if (interval_start == n + 1) /// n - 1 == interval_start - 2, n == interval_start - 1
                        continue;

                    if (interval_start != n) /// otherwise n - 1 == interval_start - 1 and already correct
                        std::nth_element(array.begin() + interval_start, array.begin() + n - 1, array.end());

                    std::swap(array[n], *std::min_element(array.begin() + n, array.end()));

                    interval_start = n + 1;
                }
            }
        }
    }

    void getManyFloat(const Float64 * levels, const size_t * indices, size_t size, Float64 * result)
    {
        if (!array.empty())
        {
            for (size_t i = 0; i < size; ++i)
            {
                auto level = levels[indices[i]];
                if (level == 0. || level == 1.)
                    throw Exception("QuantileExactExclusive cannot interpolate for the percentiles 1 and 0", ErrorCodes::BAD_ARGUMENTS);

                Float64 h = level * (array.size() + 1);
                auto n = static_cast<size_t>(h);

                if (n >= array.size())
                    result[indices[i]] = array.back();
                else if (n < 1)
                    result[indices[i]] = array.front();
                else
                    result[indices[i]] = array[n - 1] + (h - n) * (array[n] - array[n - 1]);
            }
        }
        else
        {
            for (size_t i = 0; i < size; ++i)
                result[i] = std::numeric_limits<Float64>::quiet_NaN();
        }
    }
};

/// QuantileExactInclusive is equivalent to Excel PERCENTILE and PERCENTILE.INC, R-7, SciPy-(1,1)
template <typename Value>
struct QuantileExactInclusive : public QuantileExact<Value>
{
    using FloatReturnType = Float64;
    using QuantileExact<Value>::array;

    void finalize(Float64 level)
    {
        if (!array.empty())
        {
            Float64 h = level * (array.size() - 1) + 1;
            auto n = static_cast<size_t>(h);

            if (n >= array.size())
                std::swap(*std::max_element(array.begin(), array.end()), array.back());
            else if (n < 1)
                std::swap(array.front(), *std::min_element(array.begin(), array.end()));
            else
            {
                std::nth_element(array.begin(), array.begin() + n - 1, array.end());
                std::swap(array[n], *std::min_element(array.begin() + n, array.end()));
            }
        }
    }

    /// Get the value of the `level` quantile. The level must be between 0 and 1 including bounds.
    Float64 getFloat(Float64 level)
    {
        if (!array.empty())
        {
            Float64 h = level * (array.size() - 1) + 1;
            auto n = static_cast<size_t>(h);

            if (n >= array.size())
                return array.back();
            else if (n < 1)
                return array.front();

            return array[n - 1] + (h - n) * (array[n] - array[n - 1]);
        }

        return std::numeric_limits<Float64>::quiet_NaN();
    }

    void finalize(const Float64 * levels, const size_t * indices, size_t size)
    {
        if (!array.empty())
        {
            size_t interval_start = 0;
            for (size_t i = 0; i < size; ++i)
            {
                auto level = levels[indices[i]];

                Float64 h = level * (array.size() - 1) + 1;
                auto n = static_cast<size_t>(h);

                if (n >= array.size())
                    std::swap(*std::max_element(array.begin(), array.end()), array.back());
                else if (n < 1)
                    std::swap(array.front(), *std::min_element(array.begin(), array.end()));
                else
                {
                    /// we need to place correct elements on positions n - 1 and n
                    /// if elements on positions interval_start - 2 and interval_start - 1 are correct

                    if (interval_start == n + 1) /// n - 1 == interval_start - 2, n == interval_start - 1
                        continue;

                    if (interval_start != n) /// otherwise n - 1 == interval_start - 1 and already correct
                        std::nth_element(array.begin() + interval_start, array.begin() + n - 1, array.end());

                    std::swap(array[n], *std::min_element(array.begin() + n, array.end()));

                    interval_start = n + 1;
                }
            }
        }
    }

    void getManyFloat(const Float64 * levels, const size_t * indices, size_t size, Float64 * result)
    {
        if (!array.empty())
        {
            for (size_t i = 0; i < size; ++i)
            {
                auto level = levels[indices[i]];

                Float64 h = level * (array.size() - 1) + 1;
                auto n = static_cast<size_t>(h);

                if (n >= array.size())
                    result[indices[i]] = array.back();
                else if (n < 1)
                    result[indices[i]] = array.front();
                else
                    result[indices[i]] = array[n - 1] + (h - n) * (array[n] - array[n - 1]);
            }
        }
        else
        {
            for (size_t i = 0; i < size; ++i)
                result[i] = std::numeric_limits<Float64>::quiet_NaN();
        }
    }
};

}