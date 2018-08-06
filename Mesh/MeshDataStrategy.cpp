/*
	This file is part of the Rendering library.
	Copyright (C) 2007-2012 Benjamin Eikel <benjamin@eikel.org>
	Copyright (C) 2007-2012 Claudius Jähn <claudius@uni-paderborn.de>
	Copyright (C) 2007-2012 Ralf Petring <ralf@petring.net>
	Copyright (C) 2018 Sascha Brandt <sascha@brandt.graphics>
	
	This library is subject to the terms of the Mozilla Public License, v. 2.0.
	You should have received a copy of the MPL along with this library; see the 
	file LICENSE. If not, you can obtain one at http://mozilla.org/MPL/2.0/.
*/
#include "MeshDataStrategy.h"
#include "Mesh.h"
#include "MeshIndexData.h"
#include "MeshVertexData.h"
#include "../GLHeader.h"
#include "../RenderingContext/RenderingContext.h"
#include <Util/Macros.h>
#include <iostream>

namespace Rendering {

//! (static)
MeshDataStrategy * MeshDataStrategy::defaultStrategy = nullptr;

//! (static)
MeshDataStrategy * MeshDataStrategy::getDefaultStrategy(){
	if(defaultStrategy == nullptr) {
		defaultStrategy = SimpleMeshDataStrategy::getStaticDrawReleaseLocalStrategy();
	}
	return defaultStrategy;
}

//! (static)
void MeshDataStrategy::setDefaultStrategy(MeshDataStrategy * newDefault){
	defaultStrategy = newDefault;
}

// -------------

//! (static,internal)
void MeshDataStrategy::doDisplayMesh(RenderingContext & context, Mesh * m, uint32_t startIndex, uint32_t indexCount){
	MeshVertexData & vd=m->_getVertexData();
	if(!vd.isUploaded()) vd.upload();
	
	context.setVertexFormat(0, vd.getVertexDescription());
	context.bindVertexBuffer(0, vd.getGLId(), vd.getOffset(), vd.getElementSize());
	if(m->isUsingIndexData()) {
		MeshIndexData & id=m->_getIndexData();
		if(!id.isUploaded()) id.upload();
		context.bindIndexBuffer(id.getGLId());
		context.drawElements(m->getGLDrawMode(), GL_UNSIGNED_INT, startIndex, indexCount);		
		context.bindIndexBuffer(0);
	} else {
		context.drawArrays(m->getGLDrawMode(), startIndex, indexCount);
	}
	context.bindVertexBuffer(0, 0, 0, 1);
}

// ------------------------------------------------------------------------------------

//! (static)
SimpleMeshDataStrategy * SimpleMeshDataStrategy::getStaticDrawReleaseLocalStrategy(){
	static SimpleMeshDataStrategy strategy( 0 );
	return &strategy;
}

//! (static)
SimpleMeshDataStrategy * SimpleMeshDataStrategy::getDebugStrategy(){
	static SimpleMeshDataStrategy strategy( DEBUG_OUTPUT );
	return &strategy;
}

//! (static)
SimpleMeshDataStrategy * SimpleMeshDataStrategy::getStaticDrawPreserveLocalStrategy(){
	static SimpleMeshDataStrategy strategy( PRESERVE_LOCAL_DATA );
	return &strategy;
}

//! (static)
SimpleMeshDataStrategy * SimpleMeshDataStrategy::getDynamicVertexStrategy(){
	static SimpleMeshDataStrategy strategy( PRESERVE_LOCAL_DATA|DYNAMIC_VERTICES );
	return &strategy;
}

//! (static)
SimpleMeshDataStrategy * SimpleMeshDataStrategy::getPureLocalStrategy(){
	static SimpleMeshDataStrategy strategy( CLIENT_STORAGE|PRESERVE_LOCAL_DATA|DYNAMIC_VERTICES);
	return &strategy;
}

// ----

/*! (ctor)	*/
SimpleMeshDataStrategy::SimpleMeshDataStrategy(const uint8_t _flags ) :
		flags(_flags) {
	//ctor
}

/*! (dtor)	*/
SimpleMeshDataStrategy::~SimpleMeshDataStrategy(){
//	std::cout << " ~ds ";
	//dtor
}


//! ---|> MeshDataStrategy
void SimpleMeshDataStrategy::assureLocalVertexData(Mesh * m){
	MeshVertexData & vd=m->_getVertexData();

	if( vd.dataSize()==0 && vd.isUploaded())
		vd.download();
}

//! ---|> MeshDataStrategy
void SimpleMeshDataStrategy::assureLocalIndexData(Mesh * m){
	MeshIndexData & id=m->_getIndexData();

	if( id.dataSize()==0 && id.isUploaded())
		id.download();
}

//! ---|> MeshDataStrategy
void SimpleMeshDataStrategy::prepare(Mesh * m){
	MeshIndexData & id=m->_getIndexData();
	if( id.empty() && id.isUploaded() ){ // "old" VBO present, although data has been removed
		if(getFlag(DEBUG_OUTPUT))	std::cout << " ~idxBO";
		id.removeGlBuffer();
	} else if( !id.empty() && (id.hasChanged() || !id.isUploaded()) ){ // data has changed or is new
		if(getFlag(DEBUG_OUTPUT))	std::cout << " +idxBO";
		id.upload(GL_STATIC_DRAW);
	}
	if(!getFlag(PRESERVE_LOCAL_DATA) && id.isUploaded() && id.hasLocalData()){
		if(getFlag(DEBUG_OUTPUT))	std::cout << " ~idxLD";
		id.releaseLocalData();
	}

	MeshVertexData & vd=m->_getVertexData();
	if( vd.empty() && vd.isUploaded() ){ // "old" VBO present, although data has been removed
		if(getFlag(DEBUG_OUTPUT))	std::cout << " ~vBO";
		vd.removeGlBuffer();
	} else if( !vd.empty() && (vd.hasChanged() || !vd.isUploaded()) ){ // data has changed or is new
		if(getFlag(DEBUG_OUTPUT))	std::cout << " +vBO";
		vd.upload( (getFlag(DYNAMIC_VERTICES) ? BufferObject::FLAGS_DYNAMIC : BufferObject::FLAGS_STATIC) | (getFlag(CLIENT_STORAGE) ? BufferObject::FLAG_CLIENT_STORAGE : 0) );
	}
	if(!getFlag(PRESERVE_LOCAL_DATA) && vd.isUploaded() && vd.hasLocalData()){
		if(getFlag(DEBUG_OUTPUT))	std::cout << " ~vLD";
		vd.releaseLocalData();
	}

}

//! ---|> MeshDataStrategy
void SimpleMeshDataStrategy::displayMesh(RenderingContext & context, Mesh * m,uint32_t startIndex,uint32_t indexCount){
	if( !m->empty() )
		MeshDataStrategy::doDisplayMesh(context,m,startIndex,indexCount);
}

}
