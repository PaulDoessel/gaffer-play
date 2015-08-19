//////////////////////////////////////////////////////////////////////////
//
//  Copyright (c) 2015, Image Engine Design Inc. All rights reserved.
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
//      * Neither the name of Image Engine Design nor the names of
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

#include <iostream>

#include "OpenImageIO/fmath.h"
#include "OpenImageIO/filter.h"

#include "Gaffer/Context.h"
#include "Gaffer/StringPlug.h"

#include "GafferImage/Resample.h"
#include "GafferImage/Sampler.h"

using namespace Imath;
using namespace IECore;
using namespace Gaffer;
using namespace GafferImage;

//////////////////////////////////////////////////////////////////////////
// Utilities
//////////////////////////////////////////////////////////////////////////

namespace
{

// Used as a bitmask to say which filter pass(es) we're computing.
enum Passes
{
	Horizontal = 1,
	Vertical = 2,
	Both = Horizontal | Vertical
};

unsigned requiredPasses( const Resample *resample, const ImagePlug *image, const OIIO::Filter2D *filter )
{
	int debug = resample->debugPlug()->getValue();
	if( debug == Resample::HorizontalPass )
	{
		return Horizontal;
	}
	else if( debug == Resample::SinglePass )
	{
		return Horizontal | Vertical;
	}

	if( image == image->parent<ImageNode>()->outPlug() )
	{
		return filter->separable() ? Vertical : Both;
	}
	return Horizontal;
}

// Rounds min down, and max up, while converting from float to int.
Box2i box2fToBox2i( const Box2f &b )
{
	return Box2i(
		V2i( floor( b.min.x ), floor( b.min.y ) ),
		V2i( ceil( b.max.x ), ceil( b.max.y ) )
	);
}

// Calculates the scale and offset needed to convert from output
// coordinates to input coordinates.
void ratioAndOffset( const Box2f &dstDataWindow, const Box2i &srcDataWindow, V2f &ratio, V2f &offset )
{
	/// \todo Adjust for #1438.
	const V2f dstSize = dstDataWindow.size() + V2i( 1 );
	const V2f srcSize = srcDataWindow.size() + V2i( 1 );

	ratio = dstSize / srcSize;
	offset = srcDataWindow.min - dstDataWindow.min / ratio;
}

// Returns the input region that will need to be sampled when
// generating a given output tile.
Box2i inputRegion( const V2i &tileOrigin, unsigned passes, const V2f &ratio, const V2f &offset, const OIIO::Filter2D *filter )
{
	Box2f outputRegion( V2f( tileOrigin ), tileOrigin + V2f( ImagePlug::tileSize() ) );
	V2f filterRadius( filter->width() / 2.0f, filter->height() / 2.0f );

	Box2f result = outputRegion;
	if( passes & Horizontal )
	{
		result.min.x = result.min.x / ratio.x + offset.x - filterRadius.x;
		result.max.x = result.max.x / ratio.x + offset.x + filterRadius.x;
	}
	if( passes & Vertical )
	{
		result.min.y = result.min.y / ratio.y + offset.y - filterRadius.y;
		result.max.y = result.max.y / ratio.y + offset.y + filterRadius.y;
	}

	return box2fToBox2i( result );
}

typedef boost::shared_ptr<OIIO::Filter2D> Filter2DPtr;
Filter2DPtr createFilter( const std::string &name, const V2f &filterWidth, const V2f &ratio )
{
	const char *filterName = name.c_str();
	if( name == "" )
	{
		if( ratio.x > 1.0f || ratio.y > 1.0f )
		{
			// Upsizing
			filterName = "blackman-harris";
		}
		else
		{
			// Downsizing
			filterName = "lanczos3";
		}
	}

	// We want to use the recommended width for the filter in question,
	// and we can only do that by looping over the table of registered
	// filters.
	for( int i = 0, e = OIIO::Filter2D::num_filters();  i < e;  ++i )
	{
		OIIO::FilterDesc fd;
		OIIO::Filter2D::get_filterdesc( i, &fd );
		if( !strcmp( fd.name, filterName ) )
		{
			// Filter width is specified in number of pixels in the output image.
			// When a specific width is requested, it is assumed to already be in
			// that space, but when we're using a default filter width we must apply
			// the appropriate scaling.
			return Filter2DPtr(
				OIIO::Filter2D::create(
					filterName,
					filterWidth.x > 0 ? filterWidth.x : ( fd.width * std::max( 1.0f, ratio.x ) ),
					filterWidth.y > 0 ? filterWidth.y : ( fd.width * std::max( 1.0f, ratio.y ) )
				),
				OIIO::Filter2D::destroy
			);
		}
	}

	throw Exception( boost::str( boost::format( "Unknown filter \"%s\"" ) % filterName ) );
}

} // namespace

//////////////////////////////////////////////////////////////////////////
// Resample
//////////////////////////////////////////////////////////////////////////

IE_CORE_DEFINERUNTIMETYPED( Resample );

size_t Resample::g_firstPlugIndex = 0;

Resample::Resample( const std::string &name )
	:   ImageProcessor( name )
{
	storeIndexOfNextChild( g_firstPlugIndex );
	addChild( new AtomicBox2fPlug( "dataWindow" ) );
	addChild( new StringPlug( "filter" ) );
	addChild( new V2fPlug( "filterWidth", Plug::In, V2f( 0 ), V2f( 0 ) ) );
	addChild( new IntPlug( "boundingMode", Plug::In, Sampler::Black, Sampler::Black, Sampler::Clamp ) );
	addChild( new IntPlug( "debug", Plug::In, Off, Off, SinglePass ) );
	addChild( new ImagePlug( "__horizontalPass", Plug::Out ) );

	// We don't ever want to change these, so we make pass-through connections.

	outPlug()->formatPlug()->setInput( inPlug()->formatPlug() );
	outPlug()->metadataPlug()->setInput( inPlug()->metadataPlug() );
	outPlug()->channelNamesPlug()->setInput( inPlug()->channelNamesPlug() );

	horizontalPassPlug()->formatPlug()->setInput( inPlug()->formatPlug() );
	horizontalPassPlug()->metadataPlug()->setInput( inPlug()->metadataPlug() );
	horizontalPassPlug()->channelNamesPlug()->setInput( inPlug()->channelNamesPlug() );

}

Resample::~Resample()
{
}

Gaffer::AtomicBox2fPlug *Resample::dataWindowPlug()
{
	return getChild<AtomicBox2fPlug>( g_firstPlugIndex );
}

const Gaffer::AtomicBox2fPlug *Resample::dataWindowPlug() const
{
	return getChild<AtomicBox2fPlug>( g_firstPlugIndex );
}

Gaffer::StringPlug *Resample::filterPlug()
{
	return getChild<StringPlug>( g_firstPlugIndex + 1 );
}

const Gaffer::StringPlug *Resample::filterPlug() const
{
	return getChild<StringPlug>( g_firstPlugIndex + 1 );
}

Gaffer::V2fPlug *Resample::filterWidthPlug()
{
	return getChild<V2fPlug>( g_firstPlugIndex + 2 );
}

const Gaffer::V2fPlug *Resample::filterWidthPlug() const
{
	return getChild<V2fPlug>( g_firstPlugIndex + 2 );
}

Gaffer::IntPlug *Resample::boundingModePlug()
{
	return getChild<IntPlug>( g_firstPlugIndex + 3 );
}

const Gaffer::IntPlug *Resample::boundingModePlug() const
{
	return getChild<IntPlug>( g_firstPlugIndex + 3 );
}

Gaffer::IntPlug *Resample::debugPlug()
{
	return getChild<IntPlug>( g_firstPlugIndex + 4 );
}

const Gaffer::IntPlug *Resample::debugPlug() const
{
	return getChild<IntPlug>( g_firstPlugIndex + 4 );
}

ImagePlug *Resample::horizontalPassPlug()
{
	return getChild<ImagePlug>( g_firstPlugIndex + 5 );
}

const ImagePlug *Resample::horizontalPassPlug() const
{
	return getChild<ImagePlug>( g_firstPlugIndex + 5 );
}

void Resample::affects( const Gaffer::Plug *input, AffectedPlugsContainer &outputs ) const
{
	ImageProcessor::affects( input, outputs );

	if( input == dataWindowPlug() )
	{
		outputs.push_back( outPlug()->dataWindowPlug() );
		outputs.push_back( horizontalPassPlug()->dataWindowPlug() );
	}
	else if(
		input == inPlug()->channelDataPlug() ||
		input == inPlug()->dataWindowPlug() ||
		input == dataWindowPlug() ||
		input == filterPlug() ||
		input == boundingModePlug() ||
		input->parent<V2fPlug>() == filterWidthPlug() ||
		input == debugPlug()
	)
	{
		outputs.push_back( outPlug()->channelDataPlug() );
		outputs.push_back( horizontalPassPlug()->channelDataPlug() );
	}
}

void Resample::hashDataWindow( const GafferImage::ImagePlug *parent, const Gaffer::Context *context, IECore::MurmurHash &h ) const
{
	ImageProcessor::hashDataWindow( parent, context, h );

	dataWindowPlug()->hash( h );

	if( parent == horizontalPassPlug() || debugPlug()->getValue() == HorizontalPass )
	{
		inPlug()->dataWindowPlug()->hash( h );
	}
}

Imath::Box2i Resample::computeDataWindow( const Gaffer::Context *context, const ImagePlug *parent ) const
{
	Box2i dataWindow = box2fToBox2i( dataWindowPlug()->getValue() );
	if( parent  == horizontalPassPlug() || debugPlug()->getValue() == HorizontalPass)
	{
		Box2i inDataWindow = inPlug()->dataWindowPlug()->getValue();
		dataWindow.min.y = inDataWindow.min.y;
		dataWindow.max.y = inDataWindow.max.y;
	}

	return dataWindow;
}

void Resample::hashChannelData( const GafferImage::ImagePlug *parent, const Gaffer::Context *context, IECore::MurmurHash &h ) const
{
	ImageProcessor::hashChannelData( parent, context, h );

	const Box2i srcDataWindow = inPlug()->dataWindowPlug()->getValue();
	const Box2f dstDataWindow = dataWindowPlug()->getValue();

	V2f ratio, offset;
	ratioAndOffset( dstDataWindow, srcDataWindow, ratio, offset );

	const Filter2DPtr filter = createFilter( filterPlug()->getValue(), filterWidthPlug()->getValue(), ratio );
	h.append( filter->name().c_str() );

	const unsigned passes = requiredPasses( this, parent, filter.get() );
	if( passes & Horizontal )
	{
		h.append( filter->width() );
		h.append( ratio.x );
		h.append( offset.x );
	}
	if( passes & Vertical )
	{
		h.append( filter->height() );
		h.append( ratio.y );
		h.append( offset.y );
	}

	const V2i tileOrigin = context->get<V2i>( ImagePlug::tileOriginContextName );
	Sampler sampler(
		passes == Vertical ? horizontalPassPlug() : inPlug(),
		context->get<std::string>( ImagePlug::channelNameContextName ),
		inputRegion( tileOrigin, passes, ratio, offset, filter.get() ),
		(Sampler::BoundingMode)boundingModePlug()->getValue()
	);
	sampler.hash( h );

	// Another tile might happen to need to filter over the same input
	// tiles as this one, so we must include the tile origin to make sure
	// each tile has a unique hash.
	h.append( tileOrigin );
}

IECore::ConstFloatVectorDataPtr Resample::computeChannelData( const std::string &channelName, const Imath::V2i &tileOrigin, const Gaffer::Context *context, const ImagePlug *parent ) const
{
	V2f ratio, offset;
	ratioAndOffset( dataWindowPlug()->getValue(), inPlug()->dataWindowPlug()->getValue(), ratio, offset );

	Filter2DPtr filter = createFilter( filterPlug()->getValue(), filterWidthPlug()->getValue(), ratio );
	const unsigned passes = requiredPasses( this, parent, filter.get() );

	Sampler sampler(
		passes == Vertical ? horizontalPassPlug() : inPlug(),
		channelName,
		inputRegion( tileOrigin, passes, ratio, offset, filter.get() ),
		(Sampler::BoundingMode)boundingModePlug()->getValue()
	);

	const V2i filterRadius(
		ceilf( filter->width() / ( 2.0f * ratio.x ) ),
		ceilf( filter->height() / ( 2.0f * ratio.y ) )
	);

	const Box2i tileBound( tileOrigin, tileOrigin + V2i( ImagePlug::tileSize() ) );

	FloatVectorDataPtr resultData = new FloatVectorData;
	resultData->writable().resize( ImagePlug::tileSize() * ImagePlug::tileSize() );
	std::vector<float>::iterator pIt = resultData->writable().begin();

	if( passes == Both )
	{
		// When the filter isn't separable we must perform all the
		// filtering in a single pass. This version also provides
		// a reference implementation against which the two-pass
		// version can be validated - use the SinglePass debug mode
		// to force the use of this code path.

		V2i oP; // output pixel position
		V2f iP; // input pixel position (floating point)
		V2i iPI; // input pixel position (floored to int)
		V2f iPF; // fractional part of input pixel position after flooring

		for( oP.y = tileBound.min.y; oP.y < tileBound.max.y; ++oP.y )
		{
			iP.y = ( oP.y + 0.5 ) / ratio.y + offset.y;
			iPF.y = OIIO::floorfrac( iP.y, &iPI.y );

			for( oP.x = tileBound.min.x; oP.x < tileBound.max.x; ++oP.x )
			{
				iP.x = ( oP.x + 0.5 ) / ratio.x + offset.x;
				iPF.x = OIIO::floorfrac( iP.x, &iPI.x );

				V2i fP; // relative filter position
				float v = 0.0f;
				float totalW = 0.0f;
				for( fP.y = -filterRadius.y; fP.y<= filterRadius.y; ++fP.y )
				{
					for( fP.x = -filterRadius.x; fP.x<= filterRadius.x; ++fP.x )
					{
						/// \todo version of sample taking V2i.
						const float w = (*filter)(
							ratio.x * (fP.x - ( iPF.x - 0.5f )),
							ratio.y * (fP.y - ( iPF.y - 0.5f ))
						);

						if( w == 0.0f )
						{
							continue;
						}

						v += w * sampler.sample( iPI.x + fP.x, iPI.y + fP.y );
						totalW += w;
					}
				}

				if( totalW > 0.0f )
				{
					*pIt = v / totalW;
				}

				++pIt;
			}
		}
	}
	else if( passes == Horizontal )
	{
		// When the filter is separable we can perform filtering in two
		// passes, one for the horizontal and one for the vertical. We
		// output the horizontal pass on the horizontalPassPlug() so that
		// it is cached for use in the vertical pass. The HorizontalPass
		// debug mode causes this pass to be output directly for inspection.

		V2i oP; // output pixel position
		float iX; // input pixel x coordinate (floating point)
		int iXI; // input pixel position (floored to int)
		float iXF; // fractional part of input pixel position after flooring

		for( oP.y = tileBound.min.y; oP.y < tileBound.max.y; ++oP.y )
		{
			for( oP.x = tileBound.min.x; oP.x < tileBound.max.x; ++oP.x )
			{
				iX = ( oP.x + 0.5 ) / ratio.x + offset.x;
				iXF = OIIO::floorfrac( iX, &iXI );

				int fX; // relative filter position
				float v = 0.0f;
				float totalW = 0.0f;
				for( fX = -filterRadius.x; fX<= filterRadius.x; ++fX )
				{
					const float w = filter->xfilt( ratio.x * (fX - ( iXF - 0.5f ) ) );
					if( w == 0.0f )
					{
						continue;
					}

					v += w * sampler.sample( iXI + fX, oP.y );
					totalW += w;
				}

				if( totalW > 0.0f )
				{
					*pIt = v / totalW;
				}

				++pIt;
			}
		}
	}
	else if( passes == Vertical )
	{
		V2i oP; // output pixel position
		float iY; // input pixel position (floating point)
		int iYI; // input pixel position (floored to int)
		float iYF; // fractional part of input pixel position after flooring

		for( oP.y = tileBound.min.y; oP.y < tileBound.max.y; ++oP.y )
		{
			iY = ( oP.y + 0.5 ) / ratio.y + offset.y;
			iYF = OIIO::floorfrac( iY, &iYI );

			for( oP.x = tileBound.min.x; oP.x < tileBound.max.x; ++oP.x )
			{
				int fY; // relative filter position
				float v = 0.0f;
				float totalW = 0.0f;
				for( fY = -filterRadius.y; fY<= filterRadius.y; ++fY )
				{
					const float w = filter->yfilt( ratio.y * (fY - ( iYF - 0.5f ) ) );
					if( w == 0.0f )
					{
						continue;
					}

					v += w * sampler.sample( oP.x, iYI + fY );
					totalW += w;
				}

				if( totalW > 0.0f )
				{
					*pIt = v / totalW;
				}

				++pIt;
			}
		}
	}

	return resultData;
}
