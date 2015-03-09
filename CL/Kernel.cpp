/*
	This file is part of the Rendering library.
	Copyright (C) 2014 Sascha Brandt <myeti@mail.upb.de>

	This library is subject to the terms of the Mozilla Public License, v. 2.0.
	You should have received a copy of the MPL along with this library; see the
	file LICENSE. If not, you can obtain one at http://mozilla.org/MPL/2.0/.
*/
#ifdef RENDERING_HAS_LIB_OPENCL
#include "Kernel.h"

#include "Context.h"
#include "Program.h"
#include "Device.h"
#include "Memory/Memory.h"
#include "Memory/Sampler.h"
#include "CLUtils.h"

COMPILER_WARN_PUSH
COMPILER_WARN_OFF(-Wpedantic)
COMPILER_WARN_OFF(-Wold-style-cast)
COMPILER_WARN_OFF(-Wcast-qual)
COMPILER_WARN_OFF(-Wshadow)
COMPILER_WARN_OFF(-Wstack-protector)
#include <CL/cl.hpp>
COMPILER_WARN_POP
#include <Util/Macros.h>

namespace Rendering {
namespace CL {

Kernel::Kernel(Program* _program, const std::string& name) : Util::ReferenceCounter<Kernel>(), program(_program) {
	cl_int err;
	kernel.reset(new cl::Kernel(*program->_internal(), name.c_str(), &err));
	if(err != CL_SUCCESS)
		WARN("Could not create kernel (" + getErrorString(err) + ")");
	FAIL_IF(err != CL_SUCCESS);
}

Kernel::Kernel(const Kernel& _kernel) : Util::ReferenceCounter<Kernel>(), kernel(new cl::Kernel(*_kernel.kernel.get())), program(_kernel.program) { }

Kernel::~Kernel() = default;

//Kernel::Kernel(Kernel&& kernel) = default;
//
//Kernel& Kernel::operator=(Kernel&&) = default;

bool Kernel::_setArg(uint32_t index, Memory* value) {
	cl_int err = kernel->setArg(index, *value->_internal());
	if(err != CL_SUCCESS)
		WARN("Could not set kernel argument (" + getErrorString(err) + ")");
	return err == CL_SUCCESS;
}
bool Kernel::_setArg(uint32_t index, Sampler* value) {
	cl_int err = kernel->setArg(index, *value->_internal());
	if(err != CL_SUCCESS)
		WARN("Could not set kernel argument (" + getErrorString(err) + ")");
	return err == CL_SUCCESS;
}

//bool Kernel::setArg(uint32_t index, float value) {
//	cl_int err = kernel->setArg(index, value);
//	if(err != CL_SUCCESS)
//		WARN("Could not set kernel argument (" + getErrorString(err) + ")");
//	return err == CL_SUCCESS;
//}

bool Kernel::setArg(uint32_t index, size_t size, void* ptr) {
	cl_int err = kernel->setArg(index, size, ptr);
	if(err != CL_SUCCESS)
		WARN("Could not set kernel argument (" + getErrorString(err) + ")");
	return err == CL_SUCCESS;
}

std::string Kernel::getAttributes() const {
	return kernel->getInfo<CL_KERNEL_ATTRIBUTES>();
}

std::string Kernel::getFunctionName() const {
	return kernel->getInfo<CL_KERNEL_FUNCTION_NAME>();
}

uint32_t Kernel::getNumArgs() const {
	return kernel->getInfo<CL_KERNEL_NUM_ARGS>();
}

std::string Kernel::getArgName(uint32_t index) const {
#if defined(CL_VERSION_1_2)
	return kernel->getArgInfo<CL_KERNEL_ARG_NAME>(index);
#else
	WARN("Kernel::getArgName requires at least OpenCL 1.2");
	return "";
#endif
}

std::string Kernel::getArgTypeName(uint32_t index) const {
#if defined(CL_VERSION_1_2)
	return kernel->getArgInfo<CL_KERNEL_ARG_TYPE_NAME>(index);
#else
	WARN("Kernel::getArgInfo requires at least OpenCL 1.2");
	return "";
#endif
}

//std::array<size_t, 3> Kernel::getGlobalWorkSize(Device* device) const {
//	return kernel->getWorkGroupInfo<CL_KERNEL_GLOBAL_WORK_SIZE>(*device->_internal());
//}

size_t Kernel::getWorkGroupSize(Device* device) const {
	return kernel->getWorkGroupInfo<CL_KERNEL_WORK_GROUP_SIZE>(*device->_internal());
}

std::array<size_t, 3> Kernel::getCompileWorkGroupSize(Device* device) const {
	auto sizes = kernel->getWorkGroupInfo<CL_KERNEL_COMPILE_WORK_GROUP_SIZE>(*device->_internal());
	std::array<size_t, 3> out;
	out[0] = sizes[0];
	out[1] = sizes[1];
	out[2] = sizes[2];
	return out;
}

uint64_t Kernel::getLocalMemSize(Device* device) const {
	return kernel->getWorkGroupInfo<CL_KERNEL_LOCAL_MEM_SIZE>(*device->_internal());
}

size_t Kernel::getPreferredWorkGroupSizeMultiple(Device* device) const {
	return kernel->getWorkGroupInfo<CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE>(*device->_internal());
}

uint64_t Kernel::getPrivateMemSize(Device* device) const {
	return kernel->getWorkGroupInfo<CL_KERNEL_PRIVATE_MEM_SIZE>(*device->_internal());
}

} /* namespace CL */
} /* namespace Rendering */

#endif /* RENDERING_HAS_LIB_OPENCL */
