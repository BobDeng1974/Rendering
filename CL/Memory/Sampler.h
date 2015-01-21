/*
 This file is part of the Rendering library.
 Copyright (C) 2014 Sascha Brandt <myeti@mail.upb.de>

 This library is subject to the terms of the Mozilla Public License, v. 2.0.
 You should have received a copy of the MPL along with this library; see the
 file LICENSE. If not, you can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifdef RENDERING_HAS_LIB_OPENCL
#ifndef RENDERING_CL_SAMPLER_H_
#define RENDERING_CL_SAMPLER_H_

#include <memory>

namespace cl {
class Sampler;
}

namespace Rendering {
namespace CL {
class Context;

class Sampler {
public:
	enum AdressingMode_r {
		None, MirroredRepeat, Repeat, ClampToEdge, Clamp
	};
	enum FilterMode_t {
		Nearest, Linear
	};

	Sampler();
	Sampler(Context* context, bool normalizedCoords, AdressingMode_r addressingMode, FilterMode_t filterMode);
	Sampler(const Sampler& buffer);
	Sampler(Sampler&& sampler);
	Sampler& operator=(Sampler&&);
	~Sampler();

	Context* getContext() const;
	AdressingMode_r getAdressingMode() const;
	FilterMode_t getFilterMode() const;
	bool hasNormalizedCoords() const;

	cl::Sampler* _internal() const { return sampler.get(); }
private:
	std::unique_ptr<cl::Sampler> sampler;
};

} /* namespace CL */
} /* namespace Rendering */

#endif /* RENDERING_CL_SAMPLER_H_ */
#endif /* RENDERING_HAS_LIB_OPENCL */
