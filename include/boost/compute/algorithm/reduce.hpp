//---------------------------------------------------------------------------//
// Copyright (c) 2013 Kyle Lutz <kyle.r.lutz@gmail.com>
//
// Distributed under the Boost Software License, Version 1.0
// See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt
//
// See http://kylelutz.github.com/compute for more information.
//---------------------------------------------------------------------------//

#ifndef BOOST_COMPUTE_ALGORITHM_REDUCE_HPP
#define BOOST_COMPUTE_ALGORITHM_REDUCE_HPP

#include <iterator>

#include <boost/utility/result_of.hpp>

#include <boost/compute/system.hpp>
#include <boost/compute/functional.hpp>
#include <boost/compute/detail/meta_kernel.hpp>
#include <boost/compute/command_queue.hpp>
#include <boost/compute/container/vector.hpp>
#include <boost/compute/algorithm/copy_n.hpp>
#include <boost/compute/algorithm/detail/inplace_reduce.hpp>
#include <boost/compute/algorithm/detail/reduce_on_gpu.hpp>
#include <boost/compute/algorithm/detail/serial_reduce.hpp>
#include <boost/compute/detail/iterator_range_size.hpp>

namespace boost {
namespace compute {
namespace detail {

template<class InputIterator, class OutputIterator, class BinaryFunction>
size_t reduce(InputIterator first,
              size_t count,
              OutputIterator result,
              size_t block_size,
              BinaryFunction function,
              command_queue &queue)
{
    typedef typename
        std::iterator_traits<InputIterator>::value_type
        input_type;
    typedef typename
        boost::tr1_result_of<BinaryFunction(input_type, input_type)>::type
        result_type;

    const context &context = queue.get_context();
    size_t block_count = count / 2 / block_size;
    size_t total_block_count =
        static_cast<size_t>(std::ceil(float(count) / 2.f / float(block_size)));

    if(block_count != 0){
        meta_kernel k("block_reduce");
        size_t output_arg = k.add_arg<result_type *>("__global", "output");
        size_t block_arg = k.add_arg<input_type *>("__local", "block");

        k <<
            "const uint gid = get_global_id(0);\n" <<
            "const uint lid = get_local_id(0);\n" <<

            // copy values to local memory
            "block[lid] = " <<
                function(first[k.make_var<uint_>("gid*2+0")],
                         first[k.make_var<uint_>("gid*2+1")]) << ";\n" <<

            // perform reduction
            "for(uint i = 1; i < " << uint_(block_size) << "; i <<= 1){\n" <<
            "    barrier(CLK_LOCAL_MEM_FENCE);\n" <<
            "    uint mask = (i << 1) - 1;\n" <<
            "    if((lid & mask) == 0){\n" <<
            "        block[lid] = " <<
                         function(k.expr<input_type>("block[lid]"),
                                  k.expr<input_type>("block[lid+i]")) << ";\n" <<
            "    }\n" <<
            "}\n" <<

            // write block result to global output
            "if(lid == 0)\n" <<
            "    output[get_group_id(0)] = block[0];\n";

        kernel kernel = k.compile(context);
        kernel.set_arg(output_arg, result.get_buffer());
        kernel.set_arg(block_arg, block_size * sizeof(input_type), 0);

        queue.enqueue_1d_range_kernel(kernel,
                                      0,
                                      block_count * block_size,
                                      block_size);
    }

    // serially reduce any leftovers
    if(block_count * block_size * 2 < count){
        size_t last_block_start = block_count * block_size * 2;

        meta_kernel k("extra_serial_reduce");
        size_t count_arg = k.add_arg<uint_>("count");
        size_t offset_arg = k.add_arg<uint_>("offset");
        size_t output_arg = k.add_arg<result_type *>("__global", "output");
        size_t output_offset_arg = k.add_arg<uint_>("output_offset");

        k <<
            k.decl<result_type>("result") << " = \n" <<
                first[k.expr<uint_>("offset")] << ";\n" <<
            "for(uint i = offset + 1; i < count; i++)\n" <<
            "    result = " <<
                     function(k.var<result_type>("result"),
                              first[k.var<uint_>("i")]) << ";\n" <<
            "output[output_offset] = result;\n";

        kernel kernel = k.compile(context);
        kernel.set_arg(count_arg, static_cast<uint_>(count));
        kernel.set_arg(offset_arg, static_cast<uint_>(last_block_start));
        kernel.set_arg(output_arg, result.get_buffer());
        kernel.set_arg(output_offset_arg, static_cast<uint_>(block_count));

        queue.enqueue_task(kernel);
    }

    return total_block_count;
}

template<class InputIterator, class BinaryFunction>
inline vector<
    typename boost::tr1_result_of<
        BinaryFunction(
            typename std::iterator_traits<InputIterator>::value_type,
            typename std::iterator_traits<InputIterator>::value_type
        )
    >::type
>
block_reduce(InputIterator first,
             size_t count,
             size_t block_size,
             BinaryFunction function,
             command_queue &queue)
{
    typedef typename
        std::iterator_traits<InputIterator>::value_type
        input_type;
    typedef typename
        boost::tr1_result_of<BinaryFunction(input_type, input_type)>::type
        result_type;

    const context &context = queue.get_context();
    size_t total_block_count =
        static_cast<size_t>(std::ceil(float(count) / 2.f / float(block_size)));
    vector<result_type> result_vector(total_block_count, context);

    reduce(first, count, result_vector.begin(), block_size, function, queue);

    return result_vector;
}

template<class InputIterator, class OutputIterator, class BinaryFunction>
inline void generic_reduce(InputIterator first,
                           InputIterator last,
                           OutputIterator result,
                           BinaryFunction function,
                           command_queue &queue)
{
    typedef typename
        std::iterator_traits<InputIterator>::value_type
        input_type;
    typedef typename
        boost::tr1_result_of<BinaryFunction(input_type, input_type)>::type
        result_type;

    const device &device = queue.get_device();
    const context &context = queue.get_context();

    size_t count = detail::iterator_range_size(first, last);
    if(count == 0){
        return;
    }

    boost::compute::vector<result_type> value(1, context);

    if(device.type() & device::cpu){
        detail::serial_reduce(first, last, value.begin(), function, queue);
        boost::compute::copy_n(value.begin(), 1, result, queue);
    }
    else {
        size_t block_size = 256;

        // first pass
        vector<result_type> results = detail::block_reduce(first,
                                                           count,
                                                           block_size,
                                                           function,
                                                           queue);

        if(results.size() > 1){
            detail::inplace_reduce(results.begin(),
                                   results.end(),
                                   function,
                                   queue);
        }

        boost::compute::copy_n(results.begin(), 1, result, queue);
    }
}

template<class T>
inline void dispatch_reduce(const buffer_iterator<T> first,
                            const buffer_iterator<T> last,
                            const buffer_iterator<T> result,
                            const plus<T> &function,
                            command_queue &queue)
{
    const device &device = queue.get_device();
    if(device.type() & device::gpu && first.get_index() == 0){
        reduce_on_gpu(first, last, result, queue);
    }
    else {
        generic_reduce(first, last, result, function, queue);
    }
}

template<class InputIterator, class OutputIterator, class BinaryFunction>
inline void dispatch_reduce(InputIterator first,
                            InputIterator last,
                            OutputIterator result,
                            BinaryFunction function,
                            command_queue &queue)
{
    generic_reduce(first, last, result, function, queue);
}

} // end detail namespace

/// Returns the sum of the elements in the range [\p first, \p last). This
/// is equivalent to calling reduce() with the \c plus<T>() function.
template<class InputIterator, class OutputIterator>
inline void reduce(InputIterator first,
                   InputIterator last,
                   OutputIterator result,
                   command_queue &queue = system::default_queue())
{
    typedef typename std::iterator_traits<InputIterator>::value_type T;

    detail::dispatch_reduce(first, last, result, plus<T>(), queue);
}

/// Returns the result of applying \p function to the elements in the
/// range [\p first, \p last).
///
/// The difference between the reduce() function and the accumulate()
/// function is that reduce() requires the binary operator to be
/// commutative.
///
/// This algorithm supports both host and device iterators for the
/// result argument. This allows for values to be reduced and copied
/// to the host all with a single function call.
template<class InputIterator, class OutputIterator, class BinaryFunction>
inline void reduce(InputIterator first,
                   InputIterator last,
                   OutputIterator result,
                   BinaryFunction function,
                   command_queue &queue = system::default_queue())
{
    detail::dispatch_reduce(first, last, result, function, queue);
}

} // end compute namespace
} // end boost namespace

#endif // BOOST_COMPUTE_ALGORITHM_REDUCE_HPP
