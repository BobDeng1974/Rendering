/*
	This file is part of the Rendering library.
	Copyright (C) 2007-2013 Benjamin Eikel <benjamin@eikel.org>
	Copyright (C) 2007-2013 Claudius Jähn <claudius@uni-paderborn.de>
	Copyright (C) 2007-2012 Ralf Petring <ralf@petring.net>
	Copyright (C) 2014-2018 Sascha Brandt <sascha@brandt.graphics>
	
	This library is subject to the terms of the Mozilla Public License, v. 2.0.
	You should have received a copy of the MPL along with this library; see the 
	file LICENSE. If not, you can obtain one at http://mozilla.org/MPL/2.0/.
*/
#include "RenderingContext.h"

#include "ParameterCache.h"
#include "PipelineState.h"
#include "RenderingParameters.h"
#include "../Memory/BufferObject.h"
#include "../Memory/BufferLock.h"
#include "../Mesh/Mesh.h"
#include "../Mesh/VertexAttribute.h"
#include "../Mesh/VertexDescription.h"
#include "../Shader/Shader.h"
#include "../Shader/UniformRegistry.h"
#include "../Texture/Texture.h"
#include "../FBO.h"
#include "../GLHeader.h"
#include "../Helper.h"
#include "../VAO.h"
#include <Geometry/Matrix4x4.h>
#include <Geometry/Rect.h>
#include <Util/Graphics/ColorLibrary.h>
#include <Util/Graphics/Color.h>
#include <Util/Macros.h>
#include <Util/References.h>
#include <array>
#include <stdexcept>
#include <stack>
#include <map>
#include <set>
#include <numeric>
#include <cstring>

#ifdef WIN32
#include <GL/wglew.h>
#endif

namespace Rendering {

static const Util::StringIdentifier PARAMETER_FRAMEDATA("FrameData");
static const Util::StringIdentifier PARAMETER_OBJECTDATA("ObjectData");
static const Util::StringIdentifier PARAMETER_MATERIALDATA("MaterialData");
static const Util::StringIdentifier PARAMETER_LIGHTDATA("LightData");
static const Util::StringIdentifier PARAMETER_LIGHTSETDATA("LightSetData");
static const Util::StringIdentifier PARAMETER_TEXTURESETDATA("TextureSetData");

static const uint32_t MAX_FRAMEDATA = 1;
static const uint32_t MAX_OBJECTDATA = 512;
static const uint32_t MAX_MATERIALS = 1;
static const uint32_t MAX_LIGHTS = 256;
static const uint32_t MAX_LIGHTSETS = 1;
static const uint32_t MAX_TEXTURESETS = 1;
static const uint32_t MAX_ENABLED_LIGHTS = 8;

struct FrameData {
	Geometry::Matrix4x4f matrix_worldToCamera;
	Geometry::Matrix4x4f matrix_cameraToWorld;
	Geometry::Matrix4x4f matrix_cameraToClipping;
	Geometry::Matrix4x4f matrix_clippingToCamera;
	Geometry::Vec4 viewport;
};

struct ObjectData {
	Geometry::Matrix4x4f matrix_modelToCamera;
	PointParameters pointSize = 1;
	uint32_t materialId = 0;
	uint32_t lightSetId = 0;
	uint32_t drawId = 0;
};

struct LightSet {
	uint32_t count = 0;
	std::array<uint32_t, MAX_ENABLED_LIGHTS> lights;
};

struct MaterialData {
	MaterialData() {}
	MaterialData(const MaterialParameters& mat) : mat(mat) {}
	MaterialParameters mat; // 4*vec4 + 1*float -> needs 3 word padding
	uint32_t enabled = true;
	uint64_t _pad = 0;
};

typedef std::array<uint32_t, MAX_TEXTURES> TextureSet;

struct LightCompare {	
	bool operator()(const LightParameters& l1, const LightParameters& l2) const noexcept {
		return std::memcmp(&l1, &l2, sizeof(LightParameters)) < 0;
	}
};

class RenderingContext::InternalData {
	public:
		ParameterCache cache;
		PipelineState targetPipelineState;
		PipelineState activePipelineState;

		// pipeline state
		std::stack<BlendingParameters> blendingParameterStack;
		std::stack<ColorBufferParameters> colorBufferParameterStack;
		std::stack<CullFaceParameters> cullFaceParameterStack;
		std::stack<DepthBufferParameters> depthBufferParameterStack;
		std::array<std::stack<ImageBindParameters>, MAX_BOUND_IMAGES> imageStacks;
		std::array<ImageBindParameters, MAX_BOUND_IMAGES> boundImages;
		std::stack<LineParameters> lineParameterStack;
		std::stack<PolygonModeParameters> polygonModeParameterStack;
		std::stack<PolygonOffsetParameters> polygonOffsetParameterStack;
		std::stack<ScissorParameters> scissorParametersStack;
		std::stack<StencilParameters> stencilParameterStack;
						
		std::stack<Util::Reference<FBO>> fboStack;
		std::stack<Util::Reference<Shader>> shaderStack;

		UniformRegistry globalUniforms;
		
		// per frame data
		std::stack<Geometry::Matrix4x4> projectionMatrixStack;
		std::stack<Geometry::Rect_i> viewportStack;
		FrameData activeFrameData;
		
		// per object data
		std::stack<Geometry::Matrix4x4> matrixStack;
		std::stack<PointParameters> pointParameterStack;
		ObjectData activeObjectData;
		BufferLockManager objLock;
		
		// materials
		std::stack<MaterialData> materialStack;
		MaterialData activeMaterial;
		
		// lights
		std::map<LightParameters, uint8_t, LightCompare> lightRegistry;
		std::set<uint8_t> freeLightIds;
		LightSet activeLightSet;
		
		//textures
		std::array<std::stack<Util::Reference<Texture>>, MAX_TEXTURES> textureStacks;
		TextureSet enabledTextures;
		
		// other
		typedef std::pair<Util::Reference<BufferObject>,uint32_t> feedbackBufferStatus_t; // buffer->mode

		std::stack<feedbackBufferStatus_t> feedbackStack;
		feedbackBufferStatus_t activeFeedbackStatus;
		
		std::unordered_map<uint32_t,std::stack<Util::Reference<Texture>>> atomicCounterStacks; 
		
		Geometry::Rect_i windowClientArea;
};

RenderingContext::RenderingContext() :
	internalData(new InternalData), displayMeshFn() {

	resetDisplayMeshFn();

	setBlending(BlendingParameters());
	setColorBuffer(ColorBufferParameters());
	// Initially enable the back-face culling
	setCullFace(CullFaceParameters::CULL_BACK);
	// Initially enable the depth test.
	setDepthBuffer(DepthBufferParameters(true, true, Comparison::LESS));
	
	setLine(LineParameters());
	setPointParameters(PointParameters());
	setPolygonOffset(PolygonOffsetParameters());
	setStencil(StencilParameters());
	
	internalData->targetPipelineState.setVertexArray(new VAO);
	
	// initialize default caches
	auto& cache = internalData->cache;
	cache.createCache(PARAMETER_FRAMEDATA, sizeof(FrameData), MAX_FRAMEDATA, BufferObject::FLAGS_DYNAMIC);
	cache.createCache(PARAMETER_OBJECTDATA, sizeof(ObjectData), MAX_OBJECTDATA, BufferObject::FLAGS_STREAM, 2);
	cache.createCache(PARAMETER_MATERIALDATA, sizeof(MaterialData), MAX_MATERIALS, BufferObject::FLAGS_DYNAMIC);
	cache.createCache(PARAMETER_LIGHTDATA, sizeof(LightParameters), MAX_LIGHTS, BufferObject::FLAGS_DYNAMIC);
	cache.createCache(PARAMETER_LIGHTSETDATA, sizeof(LightSet), MAX_LIGHTSETS, BufferObject::FLAGS_DYNAMIC);
	cache.createCache(PARAMETER_TEXTURESETDATA, sizeof(TextureSet), MAX_TEXTURESETS, BufferObject::FLAGS_DYNAMIC);
	
	// initialize lights
	for(uint_fast8_t i=0; i<std::numeric_limits<uint8_t>::max(); ++i) {
		internalData->freeLightIds.insert(internalData->freeLightIds.end(), i);
	}
}

RenderingContext::~RenderingContext() = default;

void RenderingContext::resetDisplayMeshFn() {
	using namespace std::placeholders;
	displayMeshFn = std::bind(&Rendering::Mesh::_display, _2, _1, _3, _4);
}

void RenderingContext::displayMesh(Mesh * mesh){
	displayMeshFn(*this, mesh,0,mesh->isUsingIndexData()? mesh->getIndexCount() : mesh->getVertexCount());
}

void RenderingContext::clearScreenRect(const Geometry::Rect_i & rect, const Util::Color4f & color, bool _clearDepth) {
	pushAndSetScissor(ScissorParameters(rect));
	applyChanges();
	glClearColor(color.getR(), color.getG(), color.getB(), color.getA());
	glClear(GL_COLOR_BUFFER_BIT |( _clearDepth ? GL_DEPTH_BUFFER_BIT : 0));
	popScissor();
}

// static helper ***************************************************************************

//!	(static) 
void RenderingContext::clearScreen(const Util::Color4f & color) {
	applyChanges();
	glClearColor(color.getR(), color.getG(), color.getB(), color.getA());
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}


//! (static)
void RenderingContext::initGLState() {
#ifdef LIB_GLEW
	glewExperimental = GL_TRUE; // Workaround: Needed for OpenGL core profile until GLEW will be fixed.
	const GLenum err = glewInit();
	if(GLEW_OK != err) {
		WARN(std::string("GLEW Error: ") + reinterpret_cast<const char *>(glewGetErrorString(err)));
	}
	
	if(!glewIsSupported("GL_VERSION_4_5")) {
		throw std::runtime_error("RenderingContext::initGLState: Required OpenGL version 4.5 is not supported.");
	}

	glPixelStorei(GL_PACK_ALIGNMENT, 1); // allow glReadPixel for all possible resolutions

	glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
	glHint(GL_POLYGON_SMOOTH_HINT, GL_NICEST);

	glBlendEquation(GL_FUNC_ADD);
	
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
	
	// Enable the possibility to write gl_PointSize from the vertex shader.
	glEnable(GL_PROGRAM_POINT_SIZE);
#endif /* LIB_GLEW */

#ifdef WIN32
	wglSwapIntervalEXT(false);
#endif /* WIN32 */
}

//! (static)
void RenderingContext::flush() {
	applyChanges();
	glFlush();
}

//! (static)
void RenderingContext::finish() {
	applyChanges();
	glFinish();
}

void RenderingContext::barrier(uint32_t flags) {
	applyChanges();
	glMemoryBarrier(flags == 0 ? GL_ALL_BARRIER_BITS : static_cast<GLbitfield>(flags));
}
// Applying changes ***************************************************************************

void RenderingContext::applyChanges(bool forced) {
	try {
		const auto diff = internalData->activePipelineState.makeDiff(internalData->targetPipelineState, forced);
		internalData->activePipelineState = internalData->targetPipelineState;
		internalData->activePipelineState.apply(diff);
		
		//internalData->cache.wait();
		internalData->cache.setParameter(PARAMETER_FRAMEDATA, 0, internalData->activeFrameData);
		internalData->cache.setParameter(PARAMETER_MATERIALDATA, 0, internalData->activeMaterial);
		internalData->cache.setParameter(PARAMETER_LIGHTSETDATA, 0, internalData->activeLightSet);
		internalData->cache.setParameter(PARAMETER_TEXTURESETDATA, 0, internalData->enabledTextures);
		//internalData->cache.setParameter(PARAMETER_OBJECTDATA, 0, internalData->activeObjectData);
		
		if(internalData->activePipelineState.isShaderValid()) {
			auto shader = internalData->activePipelineState.getShader();
			for(const auto& e : shader->getInterfaceBlocks()) {
				const auto& block = e.second;
				if(block.location >= 0 && internalData->cache.isCache(block.name)) {
					internalData->cache.bind(block.name, block.location, block.target, forced);
				}
			}

			// transfer updated global uniforms to the shader
			shader->_getUniformRegistry()->performGlobalSync(internalData->globalUniforms, false);

			// apply uniforms
			shader->applyUniforms(forced);
			GET_GL_ERROR();
		}
	} catch(const std::exception & e) {
		WARN(std::string("Problem detected while setting rendering internalData: ") + e.what());
	}
	GET_GL_ERROR();
}

// Blending ************************************************************************************
const BlendingParameters & RenderingContext::getBlendingParameters() const {
	return internalData->targetPipelineState.getBlendingParameters();
}

void RenderingContext::pushAndSetBlending(const BlendingParameters & p) {
	pushBlending();
	setBlending(p);
}
void RenderingContext::popBlending() {
	if(internalData->blendingParameterStack.empty()) {
		WARN("popBlending: Empty Blending-Stack");
		return;
	}
	setBlending(internalData->blendingParameterStack.top());
	internalData->blendingParameterStack.pop();
}

void RenderingContext::pushBlending() {
	internalData->blendingParameterStack.emplace(internalData->targetPipelineState.getBlendingParameters());
}

void RenderingContext::setBlending(const BlendingParameters & p) {
	internalData->targetPipelineState.setBlendingParameters(p);
}

// ColorBuffer ************************************************************************************
const ColorBufferParameters & RenderingContext::getColorBufferParameters() const {
	return internalData->targetPipelineState.getColorBufferParameters();
}

void RenderingContext::popColorBuffer() {
	if(internalData->colorBufferParameterStack.empty()) {
		WARN("popColorBuffer: Empty ColorBuffer stack");
		return;
	}
	setColorBuffer(internalData->colorBufferParameterStack.top());
	internalData->colorBufferParameterStack.pop();
}

void RenderingContext::pushColorBuffer() {
	internalData->colorBufferParameterStack.emplace(internalData->targetPipelineState.getColorBufferParameters());
}

void RenderingContext::pushAndSetColorBuffer(const ColorBufferParameters & p) {
	pushColorBuffer();
	setColorBuffer(p);
}

void RenderingContext::setColorBuffer(const ColorBufferParameters & p) {
	internalData->targetPipelineState.setColorBufferParameters(p);
}

void RenderingContext::clearColor(const Util::Color4f & clearValue) {
	applyChanges();
	glClearColor(clearValue.getR(), clearValue.getG(), clearValue.getB(), clearValue.getA());
	glClear(GL_COLOR_BUFFER_BIT);
}

// Cull Face ************************************************************************************
const CullFaceParameters & RenderingContext::getCullFaceParameters() const {
	return internalData->targetPipelineState.getCullFaceParameters();
}
void RenderingContext::popCullFace() {
	if(internalData->cullFaceParameterStack.empty()) {
		WARN("popCullFace: Empty CullFace-Stack");
		return;
	}
	setCullFace(internalData->cullFaceParameterStack.top());
	internalData->cullFaceParameterStack.pop();
}

void RenderingContext::pushCullFace() {
	internalData->cullFaceParameterStack.emplace(internalData->targetPipelineState.getCullFaceParameters());
}

void RenderingContext::pushAndSetCullFace(const CullFaceParameters & p) {
	pushCullFace();
	setCullFace(p);
}

void RenderingContext::setCullFace(const CullFaceParameters & p) {
	internalData->targetPipelineState.setCullFaceParameters(p);
}

// DepthBuffer ************************************************************************************
const DepthBufferParameters & RenderingContext::getDepthBufferParameters() const {
	return internalData->targetPipelineState.getDepthBufferParameters();
}
void RenderingContext::popDepthBuffer() {
	if(internalData->depthBufferParameterStack.empty()) {
		WARN("popDepthBuffer: Empty DepthBuffer stack");
		return;
	}
	setDepthBuffer(internalData->depthBufferParameterStack.top());
	internalData->depthBufferParameterStack.pop();
}

void RenderingContext::pushDepthBuffer() {
	internalData->depthBufferParameterStack.emplace(internalData->targetPipelineState.getDepthBufferParameters());
}

void RenderingContext::pushAndSetDepthBuffer(const DepthBufferParameters & p) {
	pushDepthBuffer();
	setDepthBuffer(p);
}

void RenderingContext::setDepthBuffer(const DepthBufferParameters & p) {
	internalData->targetPipelineState.setDepthBufferParameters(p);
}

void RenderingContext::clearDepth(float clearValue) {
	applyChanges();
	glClearDepth(clearValue);
	glClear(GL_DEPTH_BUFFER_BIT);
}

// ImageBinding ************************************************************************************
//! (static)
bool RenderingContext::isImageBindingSupported(){
#if defined(GL_ARB_shader_image_load_store)
	static const bool support = isExtensionSupported("GL_ARB_shader_image_load_store");
	return support;
#else
	return false;
#endif
}

static void assertCorrectImageUnit(uint8_t unit){
	if(unit>=MAX_BOUND_IMAGES)
		throw std::runtime_error("RenderingContext: Invalid image unit.");
}

ImageBindParameters RenderingContext::getBoundImage(uint8_t unit)const{
	assertCorrectImageUnit(unit);
	return internalData->boundImages[unit];
}

void RenderingContext::pushBoundImage(uint8_t unit){
	assertCorrectImageUnit(unit);
	internalData->imageStacks[unit].push( internalData->boundImages[unit] );
}

void RenderingContext::pushAndSetBoundImage(uint8_t unit, const ImageBindParameters& iParam){
	pushBoundImage(unit);
	setBoundImage(unit,iParam);
}
void RenderingContext::popBoundImage(uint8_t unit){
	assertCorrectImageUnit(unit);
	auto& iStack = internalData->imageStacks[unit];
	if(iStack.empty()){
		WARN("popBoundImage: Empty stack");
	}else{
		setBoundImage(unit,iStack.top());
		iStack.pop();
	}
}

//! \note the texture in iParam may be null to unbind
void RenderingContext::setBoundImage(uint8_t unit, const ImageBindParameters& iParam){
	assertCorrectImageUnit(unit);
	internalData->boundImages[unit] = iParam;
#if defined(GL_ARB_shader_image_load_store)
	if(isImageBindingSupported()){
		GET_GL_ERROR();
		Texture* texture = iParam.getTexture();
		if(texture){
			GLenum access;
			if(!iParam.getReadOperations()){
				access =  GL_WRITE_ONLY;
			}else if(!iParam.getWriteOperations()){
				access =  GL_READ_ONLY;
			}else{
				access =  GL_READ_WRITE;
			}
			const auto& pixelFormat = texture->getFormat().pixelFormat;
			GLenum format = pixelFormat.glInternalFormat;
			// special case:the used internalFormat in TextureUtils is not applicable here
			if(pixelFormat.glLocalDataType==GL_BYTE || pixelFormat.glLocalDataType==GL_UNSIGNED_BYTE){
				if(pixelFormat.glInternalFormat==GL_RED){
					format = GL_R8;
				}else if(pixelFormat.glInternalFormat==GL_RG){
					format = GL_RG8;
				}else if(pixelFormat.glInternalFormat==GL_RGB){
					format = GL_RGB8; // not supported by opengl!
				}else if(pixelFormat.glInternalFormat==GL_RGBA){
					format = GL_RGBA8;
				}
			}
			GET_GL_ERROR();
			glBindImageTexture(unit,texture->_prepareForBinding(*this),
								iParam.getLevel(),iParam.getMultiLayer()? GL_TRUE : GL_FALSE,iParam.getLayer(), access,
								format);
			GET_GL_ERROR();
		}else{
			glBindImageTexture(unit,0,0,GL_FALSE,0, GL_READ_WRITE, GL_RGBA32F);
			GET_GL_ERROR();
		}
	}else{
		WARN("RenderingContext::setBoundImage: GL_ARB_shader_image_load_store is not supported by your driver.");
	}
#else
	WARN("RenderingContext::setBoundImage: GL_ARB_shader_image_load_store is not available for this executable.");
#endif 
}

// Line ************************************************************************************
const LineParameters& RenderingContext::getLineParameters() const {
	return internalData->targetPipelineState.getLineParameters();
}

void RenderingContext::popLine() {
	if(internalData->lineParameterStack.empty()) {
		WARN("popLine: Empty line parameters stack");
		return;
	}
	setLine(internalData->lineParameterStack.top());
	internalData->lineParameterStack.pop();
}

void RenderingContext::pushLine() {
	internalData->lineParameterStack.emplace(internalData->targetPipelineState.getLineParameters());
}

void RenderingContext::pushAndSetLine(const LineParameters & p) {
	pushLine();
	setLine(p);
}

void RenderingContext::setLine(const LineParameters & p) {
	internalData->targetPipelineState.setLineParameters(p);
}

// Point ************************************************************************************
const PointParameters& RenderingContext::getPointParameters() const {
	return internalData->activeObjectData.pointSize;
}

void RenderingContext::popPointParameters() {
	if(internalData->pointParameterStack.empty()) {
		WARN("popPoint: Empty point parameters stack");
		return;
	}
	setPointParameters(internalData->pointParameterStack.top());
	internalData->pointParameterStack.pop();
}

void RenderingContext::pushPointParameters() {
	internalData->pointParameterStack.emplace(internalData->activeObjectData.pointSize);
}

void RenderingContext::pushAndSetPointParameters(const PointParameters & p) {
	pushPointParameters();
	setPointParameters(p);
}

void RenderingContext::setPointParameters(const PointParameters & p) {
	internalData->activeObjectData.pointSize = p;
}

// PolygonMode ************************************************************************************
const PolygonModeParameters & RenderingContext::getPolygonModeParameters() const {
	return internalData->targetPipelineState.getPolygonModeParameters();
}

void RenderingContext::popPolygonMode() {
	if(internalData->polygonModeParameterStack.empty()) {
		WARN("popPolygonMode: Empty PolygonMode-Stack");
		return;
	}
	setPolygonMode(internalData->polygonModeParameterStack.top());
	internalData->polygonModeParameterStack.pop();
}

void RenderingContext::pushPolygonMode() {
	internalData->polygonModeParameterStack.emplace(internalData->targetPipelineState.getPolygonModeParameters());
}

void RenderingContext::pushAndSetPolygonMode(const PolygonModeParameters & p) {
	pushPolygonMode();
	setPolygonMode(p);
}

void RenderingContext::setPolygonMode(const PolygonModeParameters & p) {
	internalData->targetPipelineState.setPolygonModeParameters(p);
}

// PolygonOffset ************************************************************************************
const PolygonOffsetParameters & RenderingContext::getPolygonOffsetParameters() const {
	return internalData->targetPipelineState.getPolygonOffsetParameters();
}

void RenderingContext::popPolygonOffset() {
	if(internalData->polygonOffsetParameterStack.empty()) {
		WARN("popPolygonOffset: Empty PolygonOffset stack");
		return;
	}
	setPolygonOffset(internalData->polygonOffsetParameterStack.top());
	internalData->polygonOffsetParameterStack.pop();
}

void RenderingContext::pushPolygonOffset() {
	internalData->polygonOffsetParameterStack.emplace(internalData->targetPipelineState.getPolygonOffsetParameters());
}

void RenderingContext::pushAndSetPolygonOffset(const PolygonOffsetParameters & p) {
	pushPolygonOffset();
	setPolygonOffset(p);
}

void RenderingContext::setPolygonOffset(const PolygonOffsetParameters & p) {
	internalData->targetPipelineState.setPolygonOffsetParameters(p);
}

// Scissor ************************************************************************************

const ScissorParameters & RenderingContext::getScissor() const {
	return internalData->targetPipelineState.getScissorParameters();
}

void RenderingContext::popScissor() {
	if(internalData->scissorParametersStack.empty()) {
		WARN("popScissor: Empty scissor parameters stack");
		return;
	}
	setScissor(internalData->scissorParametersStack.top());
	internalData->scissorParametersStack.pop();
}

void RenderingContext::pushScissor() {
	internalData->scissorParametersStack.emplace(getScissor());
}

void RenderingContext::pushAndSetScissor(const ScissorParameters & scissorParameters) {
	pushScissor();
	setScissor(scissorParameters);
}

void RenderingContext::setScissor(const ScissorParameters & scissorParameters) {
	internalData->targetPipelineState.setScissorParameters(scissorParameters);
}


// Stencil ************************************************************************************
const StencilParameters & RenderingContext::getStencilParamters() const {
	return internalData->targetPipelineState.getStencilParameters();
}

void RenderingContext::pushAndSetStencil(const StencilParameters & stencilParameter) {
	pushStencil();
	setStencil(stencilParameter);
}

void RenderingContext::popStencil() {
	if(internalData->stencilParameterStack.empty()) {
		WARN("popStencil: Empty stencil stack");
		return;
	}
	setStencil(internalData->stencilParameterStack.top());
	internalData->stencilParameterStack.pop();
}

void RenderingContext::pushStencil() {
	internalData->stencilParameterStack.emplace(internalData->targetPipelineState.getStencilParameters());
}

void RenderingContext::setStencil(const StencilParameters & stencilParameter) {
	internalData->targetPipelineState.setStencilParameters(stencilParameter);
}

void RenderingContext::clearStencil(int32_t clearValue) {
	applyChanges();
	glClearStencil(clearValue);
	glClear(GL_STENCIL_BUFFER_BIT);
}

// FBO ************************************************************************************

FBO * RenderingContext::getActiveFBO() const {
	return internalData->targetPipelineState.getFBO().get();
}

void RenderingContext::popFBO() {
	if(internalData->fboStack.empty()) {
		WARN("popFBO: Empty FBO-Stack");
		return;
	}
	setFBO(internalData->fboStack.top().get());
	internalData->fboStack.pop();
}

void RenderingContext::pushFBO() {
	internalData->fboStack.emplace(getActiveFBO());
}

void RenderingContext::pushAndSetFBO(FBO * fbo) {
	pushFBO();
	setFBO(fbo);
}

void RenderingContext::setFBO(FBO * fbo) {
	internalData->targetPipelineState.setFBO(fbo);
}

// GLOBAL UNIFORMS ***************************************************************************
void RenderingContext::setGlobalUniform(const Uniform & u) {
	internalData->globalUniforms.setUniform(u, false, false);
}

const Uniform & RenderingContext::getGlobalUniform(const Util::StringIdentifier & uniformName) {
	return internalData->globalUniforms.getUniform(uniformName);
}

// SHADER ************************************************************************************
void RenderingContext::setShader(Shader * shader) {
	internalData->targetPipelineState.setShader(shader);
}

void RenderingContext::pushShader() {
	internalData->shaderStack.emplace(getActiveShader());
}

void RenderingContext::pushAndSetShader(Shader * shader) {
	pushShader();
	setShader(shader);
}

void RenderingContext::popShader() {
	if(internalData->shaderStack.empty()) {
		WARN("popShader: Empty Shader-Stack");
		return;
	}
	setShader(internalData->shaderStack.top().get());
}

bool RenderingContext::isShaderEnabled(Shader * shader) {
	return shader == getActiveShader();
}

Shader * RenderingContext::getActiveShader() {
	return internalData->targetPipelineState.getShader().get();
}

const Shader * RenderingContext::getActiveShader() const {
	return internalData->targetPipelineState.getShader().get();
}

void RenderingContext::dispatchCompute(uint32_t numGroupsX, uint32_t numGroupsY, uint32_t numGroupsZ) {	
	#if defined(LIB_GL) and defined(GL_ARB_compute_shader)
		if(!getActiveShader()) {
			WARN("dispatchCompute: There is no active compute shader.");
		} else {
			applyChanges();
			glDispatchCompute(numGroupsX, numGroupsY, numGroupsZ);
			GET_GL_ERROR();
		}
	#else
		WARN("dispatchCompute: Compute shaders are not supported.");
	#endif
}

void RenderingContext::dispatchComputeIndirect(size_t offset) {	
	#if defined(LIB_GL) and defined(GL_ARB_compute_shader)
		if(!getActiveShader()) {
			WARN("glDispatchComputeIndirect: There is no active compute shader.");
		} else {
			applyChanges();
			glDispatchComputeIndirect(offset);
			GET_GL_ERROR();
		}
	#else
		WARN("glDispatchComputeIndirect: Compute shaders are not supported.");
	#endif
}

void RenderingContext::loadUniformSubroutines(uint32_t shaderStage, const std::vector<uint32_t>& indices) {
	#if defined(LIB_GL) and defined(GL_ARB_shader_subroutine)
		if(!getActiveShader()) {
			WARN("loadUniformSubroutines: There is no active shader.");
		} else {
			applyChanges();
			glUniformSubroutinesuiv(shaderStage, indices.size(), static_cast<const GLuint*>(indices.data()));
			GET_GL_ERROR();
		}
	#else
		WARN("loadUniformSubroutines: Uniform subroutines are not supported.");
	#endif
}

void RenderingContext::loadUniformSubroutines(uint32_t shaderStage, const std::vector<std::string>& names) {	
	auto shader = getActiveShader();
	if(!shader) {
		WARN("loadUniformSubroutines: There is no active shader.");
	} else {
		std::vector<uint32_t> indices;
		for(const auto& name : names)
			indices.emplace_back(shader->getSubroutineIndex(shaderStage, name));
		loadUniformSubroutines(shaderStage, indices);
	}
}

void RenderingContext::_setUniformOnShader(Shader * shader, const Uniform & uniform, bool warnIfUnused, bool forced) {
	shader->_getUniformRegistry()->setUniform(uniform, warnIfUnused, forced);
}

// TEXTURES **********************************************************************************

Texture * RenderingContext::getTexture(uint8_t unit) const {
	return unit < MAX_TEXTURES ? internalData->targetPipelineState.getTexture(unit).get() : nullptr;
}

TexUnitUsageParameter RenderingContext::getTextureUsage(uint8_t unit) const {
	return internalData->enabledTextures[unit] ? TexUnitUsageParameter::TEXTURE_MAPPING : TexUnitUsageParameter::DISABLED;
}

void RenderingContext::pushTexture(uint8_t unit) {
	internalData->textureStacks.at(unit).emplace(getTexture(unit));
}

void RenderingContext::pushAndSetTexture(uint8_t unit, Texture * texture) {
	pushTexture(unit);
	setTexture(unit, texture);
}

void RenderingContext::pushAndSetTexture(uint8_t unit, Texture * texture, TexUnitUsageParameter usage) {
	pushAndSetTexture(unit, usage == TexUnitUsageParameter::DISABLED ? nullptr : texture);
}

void RenderingContext::popTexture(uint8_t unit) {
	if(internalData->textureStacks.at(unit).empty()) {
		WARN("popTexture: Empty Texture-Stack");
		return;
	}
	const auto& texture = internalData->textureStacks[unit].top();
	setTexture(unit, texture.get());
	internalData->textureStacks[unit].pop();
}

void RenderingContext::setTexture(uint8_t unit, Texture * texture) {
	Texture * oldTexture = getTexture(unit);
	if(texture != oldTexture) {
		if(texture) 
			texture->_prepareForBinding(*this);
		internalData->targetPipelineState.setTexture(unit, texture);
	}	
	internalData->enabledTextures.at(unit) = texture != nullptr;
}

void RenderingContext::setTexture(uint8_t unit, Texture * texture, TexUnitUsageParameter usage) {
	setTexture(unit, usage == TexUnitUsageParameter::DISABLED ? nullptr : texture);
}

// TRANSFORM FEEDBACK ************************************************************************

//! (static)
bool RenderingContext::isTransformFeedbackSupported(){
#if defined(GL_EXT_transform_feedback)
	static const bool support = isExtensionSupported("GL_EXT_transform_feedback");
	return support;
#else
	return false;
#endif
}

//! (static)
bool RenderingContext::requestTransformFeedbackSupport(){
	struct _{ static bool once() {
		if(RenderingContext::isTransformFeedbackSupported())
			return true;
		WARN("RenderingContext: TransformFeedback is not supported! (This warning is only shown once!)");
		return false;
	}};
	static const bool b = _::once();
	return b;
}

BufferObject * RenderingContext::getActiveTransformFeedbackBuffer() const{
	return internalData->activeFeedbackStatus.first.get();
}

void RenderingContext::popTransformFeedbackBufferStatus(){
	if(internalData->feedbackStack.empty()) {
		WARN("popTransformFeedbackBufferStatus: The stack is empty.");
	}else{
		stopTransformFeedback();
		internalData->activeFeedbackStatus = internalData->feedbackStack.top();
		_startTransformFeedback(internalData->activeFeedbackStatus.second);
	}
}
void RenderingContext::pushTransformFeedbackBufferStatus(){
	internalData->feedbackStack.emplace(internalData->activeFeedbackStatus);
}
void RenderingContext::setTransformFeedbackBuffer(BufferObject * buffer){
	applyChanges();
	if(requestTransformFeedbackSupport()){
		#if defined(LIB_GL) and defined(GL_EXT_transform_feedback)
		if(buffer!=nullptr){
			buffer->bind(GL_TRANSFORM_FEEDBACK_BUFFER_EXT);
		}else{
			glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER_EXT, 0);
		}
		#endif
	}
	internalData->activeFeedbackStatus.first = buffer;
	_startTransformFeedback(internalData->activeFeedbackStatus.second); // restart
}
void RenderingContext::_startTransformFeedback(uint32_t primitiveMode){
	applyChanges();
	if(requestTransformFeedbackSupport()){
		#if defined(LIB_GL) and defined(GL_EXT_transform_feedback)
		if(primitiveMode==0){
			glEndTransformFeedbackEXT();
		}else{
			glBeginTransformFeedbackEXT(static_cast<GLenum>(primitiveMode));
		}
		#endif // defined
	}
	internalData->activeFeedbackStatus.second = primitiveMode;
}
void RenderingContext::startTransformFeedback_lines()		{	_startTransformFeedback(GL_LINES);	}
void RenderingContext::startTransformFeedback_points()		{	_startTransformFeedback(GL_POINTS);	}
void RenderingContext::startTransformFeedback_triangles()	{	_startTransformFeedback(GL_TRIANGLES);	}
void RenderingContext::stopTransformFeedback()				{	_startTransformFeedback(0);	}

// LIGHTS ************************************************************************************

uint8_t RenderingContext::enableLight(const LightParameters & light) {
	auto it = internalData->lightRegistry.find(light);	
	uint8_t id;
	if(it == internalData->lightRegistry.end()) {
		id = registerLight(light);
		internalData->lightRegistry.emplace(light, id);
	} else {
		id = it->second;
	}	
	enableLight(id);
	return id;
}

uint8_t RenderingContext::registerLight(const LightParameters & light) {
	if(internalData->freeLightIds.empty()) {
		WARN("Cannot register more lights; ignoring call.");
		return std::numeric_limits<uint8_t>::max();
	}
	uint8_t id = *internalData->freeLightIds.begin();
	internalData->freeLightIds.erase(internalData->freeLightIds.begin());
	internalData->cache.setParameter(PARAMETER_LIGHTDATA, id, light);
	return id;
}

void RenderingContext::setLight(uint8_t lightNumber, const LightParameters & light) {
	if(internalData->freeLightIds.count(lightNumber) > 0) {
		internalData->freeLightIds.erase(lightNumber);
	}
	internalData->cache.setParameter(PARAMETER_LIGHTDATA, lightNumber, light);
}

void RenderingContext::unregisterLight(uint8_t lightNumber) {
	internalData->freeLightIds.emplace(lightNumber);
}

void RenderingContext::enableLight(uint8_t lightNumber) {
	for(uint_fast8_t i=0; i<internalData->activeLightSet.count; ++i) {
		if(internalData->activeLightSet.lights[i] == lightNumber)
			return; // aleady active
	}
	if(internalData->activeLightSet.count >= MAX_ENABLED_LIGHTS) {
		WARN("Cannot enable more lights; ignoring call.");
		return;
	}
	internalData->activeLightSet.lights[internalData->activeLightSet.count++] = lightNumber;
}

void RenderingContext::disableLight(uint8_t lightNumber) {
	uint_fast8_t pos;
	for(pos=0; pos<internalData->activeLightSet.count; ++pos) {
		if(internalData->activeLightSet.lights[pos] == lightNumber) {
			internalData->activeLightSet.count--;
			std::swap(internalData->activeLightSet.lights[pos], internalData->activeLightSet.lights[internalData->activeLightSet.count]);
			return;
		}
	}
}

// PROJECTION MATRIX *************************************************************************

void RenderingContext::popMatrix_cameraToClipping() {
	if(internalData->projectionMatrixStack.empty()) {
		WARN("Cannot pop projection matrix. The stack is empty.");
		return;
	}
	internalData->activeFrameData.matrix_cameraToClipping = internalData->projectionMatrixStack.top();
	internalData->projectionMatrixStack.pop();
}

void RenderingContext::pushMatrix_cameraToClipping() {
	internalData->projectionMatrixStack.emplace(internalData->activeFrameData.matrix_cameraToClipping);
}

void RenderingContext::pushAndSetMatrix_cameraToClipping(const Geometry::Matrix4x4 & matrix) {
	pushMatrix_cameraToClipping();
	setMatrix_cameraToClipping(matrix);
}
	
void RenderingContext::setMatrix_cameraToClipping(const Geometry::Matrix4x4 & matrix) {
	internalData->activeFrameData.matrix_cameraToClipping = matrix;
	internalData->activeFrameData.matrix_clippingToCamera = matrix.inverse();
}

const Geometry::Matrix4x4 & RenderingContext::getMatrix_cameraToClipping() const {
	return internalData->activeFrameData.matrix_cameraToClipping;
}

// CAMERA MATRIX *****************************************************************************

void RenderingContext::setMatrix_cameraToWorld(const Geometry::Matrix4x4 & matrix) {
	internalData->activeFrameData.matrix_cameraToWorld = matrix;
	internalData->activeFrameData.matrix_worldToCamera = matrix.inverse();
}

const Geometry::Matrix4x4 & RenderingContext::getMatrix_worldToCamera() const {
	return internalData->activeFrameData.matrix_worldToCamera;
}

const Geometry::Matrix4x4 & RenderingContext::getMatrix_cameraToWorld() const {
	return internalData->activeFrameData.matrix_cameraToWorld;
}

// MODEL VIEW MATRIX *************************************************************************

void RenderingContext::resetMatrix() {
	internalData->activeObjectData.matrix_modelToCamera.setIdentity();
}

void RenderingContext::pushAndSetMatrix_modelToCamera(const Geometry::Matrix4x4 & matrix) {
	pushMatrix_modelToCamera();
	setMatrix_modelToCamera(matrix);
}

const Geometry::Matrix4x4 & RenderingContext::getMatrix_modelToCamera() const {
	return internalData->activeObjectData.matrix_modelToCamera;
}

void RenderingContext::pushMatrix_modelToCamera() {
	internalData->matrixStack.emplace(internalData->activeObjectData.matrix_modelToCamera);
}

void RenderingContext::multMatrix_modelToCamera(const Geometry::Matrix4x4 & matrix) {
	internalData->activeObjectData.matrix_modelToCamera *= matrix;
}

void RenderingContext::setMatrix_modelToCamera(const Geometry::Matrix4x4 & matrix) {
	internalData->activeObjectData.matrix_modelToCamera = matrix;
}

void RenderingContext::popMatrix_modelToCamera() {
	if(internalData->matrixStack.empty()) {
		WARN("Cannot pop matrix. The stack is empty.");
		return;
	}
	internalData->activeObjectData.matrix_modelToCamera = internalData->matrixStack.top();
	internalData->matrixStack.pop();
}

// MATERIAL **********************************************************************************

const MaterialParameters & RenderingContext::getMaterial() const {
	return internalData->activeMaterial.mat;
}

void RenderingContext::popMaterial() {
	if(internalData->materialStack.empty()) {
		WARN("RenderingContext.popMaterial: stack empty, ignoring call");
		return;
	}
	internalData->materialStack.pop();
	if(internalData->materialStack.empty()) {
		internalData->activeMaterial.enabled = false;
	} else {
		internalData->activeMaterial = internalData->materialStack.top();
	}
}

void RenderingContext::pushMaterial() {
	internalData->materialStack.emplace(internalData->activeMaterial);
}

void RenderingContext::pushAndSetMaterial(const MaterialParameters & material) {
	pushMaterial();
	setMaterial(material);
}

void RenderingContext::pushAndSetColorMaterial(const Util::Color4f & color) {
	MaterialParameters material;
	material.setAmbient(color);
	material.setDiffuse(color);
	material.setSpecular(Util::ColorLibrary::BLACK);
	pushAndSetMaterial(material);
}

void RenderingContext::setMaterial(const MaterialParameters & material) {
	internalData->activeMaterial = material;
}

//  **********************************************************************************

const Geometry::Rect_i & RenderingContext::getWindowClientArea() const {
	return internalData->windowClientArea;
}

const Geometry::Rect_i & RenderingContext::getViewport() const {
	return internalData->targetPipelineState.getViewport();
}

void RenderingContext::popViewport() {
	if(internalData->viewportStack.empty()) {
		WARN("Cannot pop viewport stack because it is empty. Ignoring call.");
		return;
	}
	setViewport(internalData->viewportStack.top());
	internalData->viewportStack.pop();
}
void RenderingContext::pushViewport() {
	internalData->viewportStack.emplace(getViewport());
}
void RenderingContext::setViewport(const Geometry::Rect_i & vp) {
	internalData->targetPipelineState.setViewport(vp);
	internalData->activeFrameData.viewport = Geometry::Vec4(vp.getX(), vp.getY(), vp.getWidth(), vp.getHeight());
}

void RenderingContext::pushAndSetViewport(const Geometry::Rect_i & viewport) {
	pushViewport();
	setViewport(viewport);
}

void RenderingContext::setWindowClientArea(const Geometry::Rect_i & clientArea) {
	internalData->windowClientArea = clientArea;
}

// Vertex Format **********************************************************************************

void RenderingContext::setVertexFormat(uint32_t binding, const VertexDescription& vd) {
	const auto& shader = getActiveShader();
	internalData->targetPipelineState.resetVertexFormats(binding);
	if(shader) {
		for(const auto& attr : vd.getAttributes()) {
			int32_t location = shader->getVertexAttributeLocation(attr.getNameId());
			if(location >= 0 && location < PipelineState::MAX_VERTEXATTRIBS)
				internalData->targetPipelineState.setVertexFormat(location, attr, binding);
		}
	} else {
		uint32_t location = 0;
		for(const auto& attr : vd.getAttributes())
			internalData->targetPipelineState.setVertexFormat(location++, attr, binding);
	}
}

void RenderingContext::bindVertexBuffer(uint32_t binding, uint32_t bufferId, uint32_t offset, uint32_t stride, uint32_t divisor) {
	internalData->targetPipelineState.setVertexBinding(binding, bufferId, offset, stride, divisor);
}

void RenderingContext::bindIndexBuffer(uint32_t bufferId) {
	internalData->targetPipelineState.setElementBinding(bufferId);
}

// Draw Commands **********************************************************************************

void RenderingContext::drawArrays(uint32_t mode, uint32_t first, uint32_t count) {
	applyChanges();
	
	uint32_t drawId = internalData->cache.addParameter(PARAMETER_OBJECTDATA, internalData->activeObjectData);
  glDrawArraysInstancedBaseInstance(mode, first, count, 1, drawId);
	if(drawId >= MAX_OBJECTDATA-1)
		internalData->cache.swap(PARAMETER_OBJECTDATA);
}

void RenderingContext::drawElements(uint32_t mode, uint32_t type, uint32_t first, uint32_t count) {
	applyChanges();
	
	uint32_t drawId = internalData->cache.addParameter(PARAMETER_OBJECTDATA, internalData->activeObjectData);
	glDrawElementsInstancedBaseVertexBaseInstance(mode, count, type, reinterpret_cast<uint8_t*>(first * getGLTypeSize(type)), 1, 0, drawId);
	if(drawId >= MAX_OBJECTDATA-1)
		internalData->cache.swap(PARAMETER_OBJECTDATA);
}


// Deprecated API **********************************************************************************

void RenderingContext::setAtomicCounterTextureBuffer(uint32_t index, Texture * texture) {	
	WARN("RenderingContext::setAtomicCounterTextureBuffer: setAtomicCounterTextureBuffer is deprecated. Use general buffer objects with bind/unbind.");
}

const LightingParameters & RenderingContext::getLightingParameters() const {
	static LightingParameters p;
	return p;
}

const ClipPlaneParameters & RenderingContext::getClipPlane(uint8_t index) const {
	static ClipPlaneParameters p;
	return p;
}

const AlphaTestParameters & RenderingContext::getAlphaTestParameters() const {
	static AlphaTestParameters p;
	return p;
}

}
