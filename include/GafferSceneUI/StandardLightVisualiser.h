//////////////////////////////////////////////////////////////////////////
//
//  Copyright (c) 2015, John Haddon. All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are
//  met:
//
//      * Redistributions of source code must retain the above
//        copyright notice, this list of conditions and the following
//        disclaimer.
//
//      * Redistributions in binary form must reproduce the above
//        copyright notice, this list of conditions and the following
//        disclaimer in the documentation and/or other materials provided with
//        the distribution.
//
//      * Neither the name of John Haddon nor the names of
//        any other contributors to this software may be used to endorse or
//        promote products derived from this software without specific prior
//        written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
//  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
//  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
//  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
//  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
//  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////

#ifndef GAFFERSCENEUI_STANDARDLIGHTVISUALISER_H
#define GAFFERSCENEUI_STANDARDLIGHTVISUALISER_H

#include "GafferSceneUI/LightVisualiser.h"
#include "IECore/SimpleTypedData.h"
#include "IECoreGL/Group.h"


namespace GafferSceneUI
{

/// Class for performing standard visualisations of lights,
/// by mapping shader parameters to features of the visualisation.
/// This also provides several protected utility methods for
/// making standard visualisations, so is suitable for use as
/// a base class for custom light visualisers.
class StandardLightVisualiser : public LightVisualiser
{

	public :

		IE_CORE_DECLAREMEMBERPTR( StandardLightVisualiser )

		StandardLightVisualiser();
		virtual ~StandardLightVisualiser();

		virtual IECoreGL::ConstRenderablePtr visualise( const IECore::InternedString &attributeName, const IECore::ObjectVector *shaderVector, IECoreGL::ConstStatePtr &state ) const;

	protected :
		static const char *faceCameraVertexSource();

		static IECoreGL::ConstRenderablePtr ray();
		static IECoreGL::ConstRenderablePtr pointRays();
		static IECoreGL::ConstRenderablePtr spotlightCone( float innerAngle, float outerAngle, float lensRadius );
		static IECoreGL::ConstRenderablePtr colorIndicator( const Imath::Color3f &color, bool indicatorFaceCamera = true );

	private:
		/// These methods are private because we haven't yet completely nailed down the API for this stuff
		static const char *environmentLightDrawFragSource();
		static const char *areaLightDrawVertexSource();
		static const char *areaLightDrawFragSource();

		static void addEnvLightVisualiser( IECoreGL::GroupPtr &output, Imath::Color3f multiplier, const std::string &textureName );
		static void addAreaLightVisualiser( IECoreGL::ConstStatePtr &state, Imath::Color3f multiplier, const std::string &textureName, bool flipNormal, bool doubleSided, bool sphericalProjection, const Imath::M44f &projectionTransform );
		static void addBasicLightVisualiser( IECore::ConstStringDataPtr type, IECoreGL::GroupPtr &output, Imath::Color3f multiplier, float coneAngle, float penumbraAngle, const std::string *penumbraType, float lensRadius );


};

IE_CORE_DECLAREPTR( StandardLightVisualiser )

} // namespace GafferSceneUI

#endif // GAFFERSCENEUI_STANDARDLIGHTVISUALISER_H
