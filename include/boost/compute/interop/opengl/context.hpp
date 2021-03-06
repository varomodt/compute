//---------------------------------------------------------------------------//
// Copyright (c) 2013-2014 Kyle Lutz <kyle.r.lutz@gmail.com>
//
// Distributed under the Boost Software License, Version 1.0
// See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt
//
// See http://kylelutz.github.com/compute for more information.
//---------------------------------------------------------------------------//

#ifndef BOOST_COMPUTE_INTEROP_OPENGL_CONTEXT_HPP
#define BOOST_COMPUTE_INTEROP_OPENGL_CONTEXT_HPP

#include <boost/throw_exception.hpp>

#include <boost/compute/device.hpp>
#include <boost/compute/system.hpp>
#include <boost/compute/context.hpp>
#include <boost/compute/exception/extension_unsupported_exception.hpp>
#include <boost/compute/interop/opengl/cl_gl.hpp>

#ifdef __linux__
#include <GL/glx.h>
#endif

namespace boost {
namespace compute {

/// Creates a OpenCL/OpenGL sharing context for the currently active
/// OpenGL context.
///
/// Throws a extension_unsupported_exception if no CL-GL sharing capable
/// devices are found.
inline context opengl_create_shared_context()
{
    // name of the OpenGL sharing extension for the system
    #if defined(__APPLE__)
    const char *cl_gl_sharing_extension = "cl_APPLE_gl_sharing";
    #elif defined(__linux__)
    const char *cl_gl_sharing_extension = "cl_khr_gl_sharing";
    #endif

    typedef cl_int(*GetGLContextInfoKHRFunction)(
        const cl_context_properties*, cl_gl_context_info, size_t, void *, size_t *
    );

    std::vector<platform> platforms = system::platforms();
    for(size_t i = 0; i < platforms.size(); i++){
        const platform &platform = platforms[i];

        // load clGetGLContextInfoKHR() extension function
        GetGLContextInfoKHRFunction GetGLContextInfoKHR =
            reinterpret_cast<GetGLContextInfoKHRFunction>(
                reinterpret_cast<unsigned long>(
                    platform.get_extension_function_address("clGetGLContextInfoKHR")
                )
            );
        if(!GetGLContextInfoKHR){
            continue;
        }

        // get OpenGL share group (needed for Apple)
        #ifdef __APPLE__
        CGLContextObj cgl_current_context = CGLGetCurrentContext();
        CGLShareGroupObj cgl_share_group = CGLGetShareGroup(cgl_current_context);
        #endif

        // create context properties listing the platform and current OpenGL display
        cl_context_properties properties[] = {
            CL_CONTEXT_PLATFORM, (cl_context_properties) platform.id(),
        #if defined(__linux__)
            CL_GL_CONTEXT_KHR, (cl_context_properties) glXGetCurrentContext(),
            CL_GLX_DISPLAY_KHR, (cl_context_properties) glXGetCurrentDisplay(),
        #elif defined(__APPLE__)
            CL_CONTEXT_PROPERTY_USE_CGL_SHAREGROUP_APPLE,
                (cl_context_properties) cgl_share_group,
        #endif
            0
        };

        // lookup current OpenCL device for current OpenGL context
        cl_device_id gpu_id;
        cl_int ret = GetGLContextInfoKHR(
            properties,
            CL_CURRENT_DEVICE_FOR_GL_CONTEXT_KHR,
            sizeof(cl_device_id),
            &gpu_id,
            0
        );
        if(ret != CL_SUCCESS){
            continue;
        }

        // create device object for the GPU and ensure it supports CL-GL sharing
        device gpu(gpu_id, false);
        if(!gpu.supports_extension(cl_gl_sharing_extension)){
            continue;
        }

        // return CL-GL sharing context
        return context(gpu, properties);
    }

    // no CL-GL sharing capable devices found
    BOOST_THROW_EXCEPTION(
        extension_unsupported_exception(cl_gl_sharing_extension)
    );
}

} // end compute namespace
} // end boost namespace

#endif // BOOST_COMPUTE_INTEROP_OPENGL_CONTEXT_HPP
