/*
	Copyright (C) 2009-2015 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

//nothing in this file should be assumed to be accurate
//
//the shape rasterizers contained herein are based on code supplied by Chris Hecker from 
//http://chrishecker.com/Miscellaneous_Technical_Articles


//TODO - due to a late change of a y-coord flipping, our winding order is wrong
//this causes us to have to flip the verts for every front-facing poly.
//a performance improvement would be to change the winding order logic
//so that this is done less frequently

#include "rasterize.h"

#include <algorithm>
#include <assert.h>
#include <math.h>
#include <string.h>

#if defined(_MSC_VER) && _MSC_VER == 1600
#define SLEEP_HACK_2011
#endif

#ifdef SLEEP_HACK_2011
#include <Windows.h>
#endif

#ifndef _MSC_VER 
#include <stdint.h>
#endif

#include "bits.h"
#include "common.h"
#include "matrix.h"
#include "render3D.h"
#include "gfx3d.h"
#include "texcache.h"
#include "MMU.h"
#include "NDSSystem.h"
#include "utils/task.h"

//#undef FORCEINLINE
//#define FORCEINLINE
//#undef INLINE
//#define INLINE

using std::min;
using std::max;
using std::swap;

template<typename T> T _min(T a, T b, T c) { return min(min(a,b),c); }
template<typename T> T _max(T a, T b, T c) { return max(max(a,b),c); }
template<typename T> T _min(T a, T b, T c, T d) { return min(_min(a,b,d),c); }
template<typename T> T _max(T a, T b, T c, T d) { return max(_max(a,b,d),c); }

static u8 modulate_table[64][64];
static u8 decal_table[32][64][64];
static u8 index_lookup_table[65];
static u8 index_start_table[8];

////optimized float floor useful in limited cases
////from http://www.stereopsis.com/FPU.html#convert
////(unfortunately, it relies on certain FPU register settings)
//int Real2Int(double val)
//{
//	const double _double2fixmagic = 68719476736.0*1.5;     //2^36 * 1.5,  (52-_shiftamt=36) uses limited precisicion to floor
//	const int _shiftamt        = 16;                    //16.16 fixed point representation,
//
//	#ifdef WORDS_BIGENDIAN
//		#define iman_				1
//	#else
//		#define iman_				0
//	#endif
//
//	val		= val + _double2fixmagic;
//	return ((int*)&val)[iman_] >> _shiftamt; 
//}

//// this probably relies on rounding settings..
//int Real2Int(float val)
//{
//	//val -= 0.5f;
//	//int temp;
//	//__asm {
//	//	fld val;
//	//	fistp temp;
//	//}
//	//return temp;
//	return 0;
//}


//doesnt work yet
static FORCEINLINE int fastFloor(float f)
{
	float temp = f + 1.f;
	int ret = (*((u32*)&temp))&0x7FFFFF;
	return ret;
}


//INLINE static void SubmitVertex(int vert_index, VERT& rawvert)
//{
//	verts[vert_index] = &rawvert;
//}

static SoftRasterizerRenderer *_SoftRastRenderer = NULL;

static FORCEINLINE int iround(float f) {
	return (int)f; //lol
}


typedef int fixed28_4;

// handle floor divides and mods correctly 
static FORCEINLINE void FloorDivMod(long Numerator, long Denominator, long &Floor, long &Mod, bool& failure)
{
	//These must be caused by invalid or degenerate shapes.. not sure yet.
	//check it out in the mario face intro of SM64
	//so, we have to take out the assert.
	//I do know that we handle SOME invalid shapes without crashing,
	//since I see them acting poppy in a way that doesnt happen in the HW.. so alas it is also incorrect.
	//This particular incorrectness is not likely ever to get fixed!

	//assert(Denominator > 0);		

	//but we have to bail out since our handling for these cases currently steps scanlines 
	//the wrong way and goes totally nuts (freezes)
	if(Denominator<=0) 
		failure = true;

	if(Numerator >= 0) {
		// positive case, C is okay
		Floor = Numerator / Denominator;
		Mod = Numerator % Denominator;
	} else {
		// Numerator is negative, do the right thing
		Floor = -((-Numerator) / Denominator);
		Mod = (-Numerator) % Denominator;
		if(Mod) {
			// there is a remainder
			Floor--; Mod = Denominator - Mod;
		}
	}
}

static FORCEINLINE fixed28_4 FloatToFixed28_4( float Value ) {
	return (fixed28_4)(Value * 16);
}
static FORCEINLINE float Fixed28_4ToFloat( fixed28_4 Value ) {
	return Value / 16.0f;
}
//inline fixed16_16 FloatToFixed16_16( float Value ) {
//	return (fixed16_6)(Value * 65536);
//}
//inline float Fixed16_16ToFloat( fixed16_16 Value ) {
//	return Value / 65536.0;
//}
static FORCEINLINE fixed28_4 Fixed28_4Mul( fixed28_4 A, fixed28_4 B ) {
	// could make this asm to prevent overflow
	return (A * B) / 16;	// 28.4 * 28.4 = 24.8 / 16 = 28.4
}
static FORCEINLINE int Ceil28_4( fixed28_4 Value ) {
	int ReturnValue;
	int Numerator = Value - 1 + 16;
	if(Numerator >= 0) {
		ReturnValue = Numerator/16;
	} else {
		// deal with negative numerators correctly
		ReturnValue = -((-Numerator)/16);
		ReturnValue -= ((-Numerator) % 16) ? 1 : 0;
	}
	return ReturnValue;
}

struct edge_fx_fl {
	edge_fx_fl() {}
	edge_fx_fl(int Top, int Bottom, VERT** verts, bool& failure);
	FORCEINLINE int Step();

	VERT** verts;
	long X, XStep, Numerator, Denominator;			// DDA info for x
	long ErrorTerm;
	int Y, Height;					// current y and vertical count
	
	struct Interpolant {
		float curr, step, stepExtra;
		FORCEINLINE void doStep() { curr += step; }
		FORCEINLINE void doStepExtra() { curr += stepExtra; }
		FORCEINLINE void initialize(float value) {
			curr = value;
			step = 0;
			stepExtra = 0;
		}
		FORCEINLINE void initialize(float top, float bottom, float dx, float dy, long XStep, float XPrestep, float YPrestep) {
			dx = 0;
			dy *= (bottom-top);
			curr = top + YPrestep * dy + XPrestep * dx;
			step = XStep * dx + dy;
			stepExtra = dx;
		}
	};
	
	static const int NUM_INTERPOLANTS = 7;
	union {
		struct {
			Interpolant invw,z,u,v,color[3];
		};
		Interpolant interpolants[NUM_INTERPOLANTS];
	};
	void FORCEINLINE doStepInterpolants() { for(int i=0;i<NUM_INTERPOLANTS;i++) interpolants[i].doStep(); }
	void FORCEINLINE doStepExtraInterpolants() { for(int i=0;i<NUM_INTERPOLANTS;i++) interpolants[i].doStepExtra(); }
};

FORCEINLINE edge_fx_fl::edge_fx_fl(int Top, int Bottom, VERT** verts, bool& failure) {
	this->verts = verts;
	Y = Ceil28_4((fixed28_4)verts[Top]->y);
	int YEnd = Ceil28_4((fixed28_4)verts[Bottom]->y);
	Height = YEnd - Y;
	X = Ceil28_4((fixed28_4)verts[Top]->x);
	int XEnd = Ceil28_4((fixed28_4)verts[Bottom]->x);
	int Width = XEnd - X; // can be negative

	// even if Height == 0, give some info for horizontal line poly
	if(Height != 0 || Width != 0)
	{
		long dN = long(verts[Bottom]->y - verts[Top]->y);
		long dM = long(verts[Bottom]->x - verts[Top]->x);
		if (dN != 0)
		{
			long InitialNumerator = (long)(dM*16*Y - dM*verts[Top]->y + dN*verts[Top]->x - 1 + dN*16);
			FloorDivMod(InitialNumerator,dN*16,X,ErrorTerm,failure);
			FloorDivMod(dM*16,dN*16,XStep,Numerator,failure);
			Denominator = dN*16;
		}
		else
		{
			XStep = Width;
			Numerator = 0;
			ErrorTerm = 0;
			Denominator = 1;
			dN = 1;
		}
	
		float YPrestep = Fixed28_4ToFloat((fixed28_4)(Y*16 - verts[Top]->y));
		float XPrestep = Fixed28_4ToFloat((fixed28_4)(X*16 - verts[Top]->x));

		float dy = 1/Fixed28_4ToFloat(dN);
		float dx = 1/Fixed28_4ToFloat(dM);
		
		invw.initialize(1/verts[Top]->w,1/verts[Bottom]->w,dx,dy,XStep,XPrestep,YPrestep);
		u.initialize(verts[Top]->u,verts[Bottom]->u,dx,dy,XStep,XPrestep,YPrestep);
		v.initialize(verts[Top]->v,verts[Bottom]->v,dx,dy,XStep,XPrestep,YPrestep);
		z.initialize(verts[Top]->z,verts[Bottom]->z,dx,dy,XStep,XPrestep,YPrestep);
		for(int i=0;i<3;i++)
			color[i].initialize(verts[Top]->fcolor[i],verts[Bottom]->fcolor[i],dx,dy,XStep,XPrestep,YPrestep);
	}
	else
	{
		// even if Width == 0 && Height == 0, give some info for pixel poly
		// example: Castlevania Portrait of Ruin, warp stone
		XStep = 1;
		Numerator = 0;
		Denominator = 1;
		ErrorTerm = 0;
		invw.initialize(1/verts[Top]->w);
		u.initialize(verts[Top]->u);
		v.initialize(verts[Top]->v);
		z.initialize(verts[Top]->z);
		for(int i=0;i<3;i++)
			color[i].initialize(verts[Top]->fcolor[i]);
	}
}

FORCEINLINE int edge_fx_fl::Step() {
	X += XStep; Y++; Height--;
	doStepInterpolants();

	ErrorTerm += Numerator;
	if(ErrorTerm >= Denominator) {
		X++;
		ErrorTerm -= Denominator;
		doStepExtraInterpolants();
	}
	return Height;
}	



static FORCEINLINE void alphaBlend(FragmentColor & dst, const FragmentColor & src)
{
	if(gfx3d.renderState.enableAlphaBlending)
	{
		if(src.a == 31 || dst.a == 0)
		{
			dst = src;
		}
		//else if(src.a == 0) { } //this is not necessary since it was handled earlier
		else
		{
			u8 alpha = src.a+1;
			u8 invAlpha = 32 - alpha;
			dst.r = (alpha*src.r + invAlpha*dst.r)>>5;
			dst.g = (alpha*src.g + invAlpha*dst.g)>>5;
			dst.b = (alpha*src.b + invAlpha*dst.b)>>5;
		}

		dst.a = max(src.a,dst.a);
	}
	else
	{
		if(src.a == 0)
		{
			//do nothing; the fragment is totally transparent
		}
		else
		{
			dst = src;
		}
	}
}

template<bool RENDERER>
class RasterizerUnit
{
protected:
	SoftRasterizerRenderer *_softRender;
	TexCacheItem *lastTexKey;
	VERT* verts[MAX_CLIPPED_VERTS];
	int polynum;
	
public:
	bool _debug_thisPoly;
	int SLI_MASK;
	int SLI_VALUE;
	
	void SetRenderer(SoftRasterizerRenderer *theRenderer)
	{
		this->_softRender = theRenderer;
	}

	struct Sampler
	{
		bool enabled;
		int width, height;
		s32 wmask, hmask;
		int wrap;
		int wshift;
		int texFormat;
		
		void setup(u32 texParam)
		{
			texFormat = (texParam>>26)&7;
			wshift = ((texParam>>20)&0x07) + 3;
			width=(1 << wshift);
			height=(8 << ((texParam>>23)&0x07));
			wmask = width-1;
			hmask = height-1;
			wrap = (texParam>>16)&0xF;
			enabled = gfx3d.renderState.enableTexturing && (texFormat!=0);
		}
		
		FORCEINLINE void clamp(s32 &val, const int size, const s32 sizemask)
		{
			if(val<0) val = 0;
			if(val>sizemask) val = sizemask;
		}
		
		FORCEINLINE void hclamp(s32 &val)
		{
			clamp(val,width,wmask);
		}
		
		FORCEINLINE void vclamp(s32 &val)
		{
			clamp(val,height,hmask);
		}
		
		FORCEINLINE void repeat(s32 &val, const int size, const s32 sizemask)
		{
			val &= sizemask;
		}
		
		FORCEINLINE void hrepeat(s32 &val)
		{
			repeat(val,width,wmask);
		}
		
		FORCEINLINE void vrepeat(s32 &val)
		{
			repeat(val,height,hmask);
		}
		
		FORCEINLINE void flip(s32 &val, const int size, const s32 sizemask)
		{
			val &= ((size<<1)-1);
			if(val>=size) val = (size<<1)-val-1;
		}
		
		FORCEINLINE void hflip(s32 &val)
		{
			flip(val,width,wmask);
		}
		
		FORCEINLINE void vflip(s32 &val)
		{
			flip(val,height,hmask);
		}
		
		FORCEINLINE void dowrap(s32 &iu, s32 &iv)
		{
			switch (wrap)
			{
				//flip none
				case 0x0: hclamp(iu); vclamp(iv); break;
				case 0x1: hrepeat(iu); vclamp(iv); break;
				case 0x2: hclamp(iu); vrepeat(iv); break;
				case 0x3: hrepeat(iu); vrepeat(iv); break;
				//flip S
				case 0x4: hclamp(iu); vclamp(iv); break;
				case 0x5: hflip(iu); vclamp(iv); break;
				case 0x6: hclamp(iu); vrepeat(iv); break;
				case 0x7: hflip(iu); vrepeat(iv); break;
				//flip T
				case 0x8: hclamp(iu); vclamp(iv); break;
				case 0x9: hrepeat(iu); vclamp(iv); break;
				case 0xA: hclamp(iu); vflip(iv); break;
				case 0xB: hrepeat(iu); vflip(iv); break;
				//flip both
				case 0xC: hclamp(iu); vclamp(iv); break;
				case 0xD: hflip(iu); vclamp(iv); break;
				case 0xE: hclamp(iu); vflip(iv); break;
				case 0xF: hflip(iu); vflip(iv); break;
			}
		}
	} sampler;

	FORCEINLINE FragmentColor sample(const float u, const float v)
	{
		static const FragmentColor white = MakeFragmentColor(63,63,63,31);
		if(!sampler.enabled) return white;

		//finally, we can use floor here. but, it is slower than we want.
		//the best solution is probably to wait until the pipeline is full of fixed point
		s32 iu = 0;
		s32 iv = 0;
		
		if (!CommonSettings.GFX3D_TXTHack)
		{
			iu = s32floor(u);
			iv = s32floor(v);
		}
		else
		{
			iu = round_s(u);
			iv = round_s(v);
		}
		
		sampler.dowrap(iu, iv);
		FragmentColor color;
		color.color = ((u32*)lastTexKey->decoded)[(iv<<sampler.wshift)+iu];
		return color;
	}

	//round function - tkd3
	float round_s(double val)
	{
		if (val > 0.0)
		{
			return floorf(val*256.0f+0.5f)/256.0f; //this value(256.0) is good result.(I think)
		}
		else
		{
			return -1.0*floorf(fabs(val)*256.0f+0.5f)/256.0f;
		}
	}
	
	FORCEINLINE void shade(const u8 polygonMode, const FragmentColor src, FragmentColor &dst, const float texCoordU, const float texCoordV)
	{
		FragmentColor texColor;
		
		switch(polygonMode)
		{
			case 0: //modulate
				texColor = sample(texCoordU, texCoordV);
				dst.r = modulate_table[texColor.r][src.r];
				dst.g = modulate_table[texColor.g][src.g];
				dst.b = modulate_table[texColor.b][src.b];
				dst.a = modulate_table[GFX3D_5TO6(texColor.a)][GFX3D_5TO6(src.a)]>>1;
				//dst.a = 28;
				//#ifdef _MSC_VER
				//if(GetAsyncKeyState(VK_SHIFT)) {
				//	//debugging tricks
				//	dst = shader.materialColor;
				//	if(GetAsyncKeyState(VK_TAB)) {
				//		u8 alpha = dst.a;
				//		dst.color = polynum*8+8;
				//		dst.a = alpha;
				//	}
				//}
				//#endif
				break;
				
			case 1: //decal
			{
				if(sampler.enabled)
				{
					texColor = sample(texCoordU, texCoordV);
					dst.r = decal_table[texColor.a][texColor.r][src.r];
					dst.g = decal_table[texColor.a][texColor.g][src.g];
					dst.b = decal_table[texColor.a][texColor.b][src.b];
					dst.a = src.a;
				}
				else
				{
					dst = src;
				}
			}
				break;
				
			case 2: //toon/highlight shading
			{
				texColor = sample(texCoordU, texCoordV);
				FragmentColor toonColor = this->_softRender->toonColor32LUT[src.r >> 1];
				
				if(gfx3d.renderState.shading == GFX3D_State::HIGHLIGHT)
				{
					dst.r = modulate_table[texColor.r][src.r];
					dst.g = modulate_table[texColor.g][src.r];
					dst.b = modulate_table[texColor.b][src.r];
					dst.a = modulate_table[GFX3D_5TO6(texColor.a)][GFX3D_5TO6(src.a)] >> 1;
					
					dst.r = min<u8>(63, (dst.r + toonColor.r));
					dst.g = min<u8>(63, (dst.g + toonColor.g));
					dst.b = min<u8>(63, (dst.b + toonColor.b));
				}
				else
				{
					dst.r = modulate_table[texColor.r][toonColor.r];
					dst.g = modulate_table[texColor.g][toonColor.g];
					dst.b = modulate_table[texColor.b][toonColor.b];
					dst.a = modulate_table[GFX3D_5TO6(texColor.a)][GFX3D_5TO6(src.a)] >> 1;
				}
				
			}
				break;
				
			case 3: //shadows
				//is this right? only with the material color?
				dst = src;
				break;
		}
	}
	
	FORCEINLINE void pixel(const PolygonAttributes &polyAttr, FragmentAttributes &dstAttributes, FragmentColor &dstColor, float r, float g, float b, float invu, float invv, float w, float z)
	{
		FragmentColor srcColor;
		FragmentColor shaderOutput;

		u32 depth;
		if (gfx3d.renderState.wbuffer)
		{
			//not sure about this
			//this value was chosen to make the skybox, castle window decals, and water level render correctly in SM64
			depth = u32floor(4096*w);
		}
		else
		{
			depth = u32floor(z*0x7FFF);
			depth <<= 9;
		}

		//if(polyAttr.decalMode)
		if (polyAttr.enableDepthTest) // Why was this originally called "decalMode"?
		{
			if (CommonSettings.GFX3D_Zelda_Shadow_Depth_Hack > 0)
			{
				if( depth < dstAttributes.depth - CommonSettings.GFX3D_Zelda_Shadow_Depth_Hack
					|| depth > dstAttributes.depth + CommonSettings.GFX3D_Zelda_Shadow_Depth_Hack)
				{
					goto depth_fail;
				}
			}
			else
			{
				if (depth != dstAttributes.depth)
				{
					goto depth_fail;
				}
			}
		}
		else
		{
			if (depth >= dstAttributes.depth)
			{
				goto depth_fail;
			}
		}
		
		//handle shadow polys
		if (polyAttr.polygonMode == 3)
		{
			if (polyAttr.polygonID == 0)
			{
				goto rejected_fragment;
			}
			else
			{
				if (dstAttributes.stencil == 0)
				{
					goto rejected_fragment;
				}	

				//shadow polys have a special check here to keep from self-shadowing when user
				//has tried to prevent it from happening
				//if this isnt here, then the vehicle select in mariokart will look terrible
				if (dstAttributes.opaquePolyID == polyAttr.polygonID)
				{
					goto rejected_fragment;
				}
			}
		}
		
		//perspective-correct the colors
		r = (r * w) + 0.5f;
		g = (g * w) + 0.5f;
		b = (b * w) + 0.5f;
		
		//this is a HACK: 
		//we are being very sloppy with our interpolation precision right now
		//and rather than fix it, i just want to clamp it
		srcColor = MakeFragmentColor(max(0U,min(63U,u32floor(r))),
									 max(0U,min(63U,u32floor(g))),
									 max(0U,min(63U,u32floor(b))),
									 polyAttr.alpha);
		
		//pixel shader
		shade(polyAttr.polygonMode, srcColor, shaderOutput, invu * w, invv * w);
		
		//we shouldnt do any of this if we generated a totally transparent pixel
		if (shaderOutput.a != 0)
		{
			//alpha test (don't have any test cases for this...? is it in the right place...?)
			if (gfx3d.renderState.enableAlphaTest)
			{
				if (shaderOutput.a < gfx3d.renderState.alphaTestRef)
					goto rejected_fragment;
			}

			//handle polyids
			bool isOpaquePixel = shaderOutput.a == 31;
			if (isOpaquePixel)
			{
				dstAttributes.opaquePolyID = polyAttr.polygonID;
				dstAttributes.isTranslucentPoly = polyAttr.isTranslucent;
				dstAttributes.isFogged = polyAttr.enableRenderFog;
				dstColor = shaderOutput;
			}
			else
			{
				//dont overwrite pixels on translucent polys with the same polyids
				if (dstAttributes.translucentPolyID == polyAttr.polygonID)
					goto rejected_fragment;
			
				//originally we were using a test case of shadows-behind-trees in sm64ds
				//but, it looks bad in that game. this is actually correct
				//if this isnt correct, then complex shape cart shadows in mario kart don't work right
				dstAttributes.translucentPolyID = polyAttr.polygonID;

				//alpha blending and write color
				alphaBlend(dstColor, shaderOutput);
				
				dstAttributes.isFogged = (dstAttributes.isFogged && polyAttr.enableRenderFog);
			}

			//depth writing
			if (isOpaquePixel || polyAttr.enableAlphaDepthWrite)
				dstAttributes.depth = depth;

		}

		//shadow cases: (need multi-bit stencil buffer to cope with all of these, especially the mariokart complex shadows)
		//1. sm64 (standing near signs and blocks)
		//2. mariokart (no glitches in shadow shape in kart selector)
		//3. mariokart (no junk beneath platform in kart selector / no shadow beneath grate floor in bowser stage)
		//(specifically, the shadows in mario kart are complicated concave shapes)

		goto done;
		depth_fail:
		if (polyAttr.polygonMode == 3 && polyAttr.polygonID == 0)
			dstAttributes.stencil++;
		rejected_fragment:
		done:
		;

		if (polyAttr.polygonMode == 3 && polyAttr.polygonID != 0 && dstAttributes.stencil)
			dstAttributes.stencil--;
	}

	//draws a single scanline
	FORCEINLINE void drawscanline(const PolygonAttributes &polyAttr, const size_t framebufferWidth, const size_t framebufferHeight, edge_fx_fl *pLeft, edge_fx_fl *pRight, bool lineHack)
	{
		int XStart = pLeft->X;
		int width = pRight->X - XStart;

		// HACK: workaround for vertical/slant line poly
		if (lineHack && width == 0)
		{
			int leftWidth = pLeft->XStep;
			if (pLeft->ErrorTerm + pLeft->Numerator >= pLeft->Denominator)
				leftWidth++;
			int rightWidth = pRight->XStep;
			if (pRight->ErrorTerm + pRight->Numerator >= pRight->Denominator)
				rightWidth++;
			width = max(1, max(abs(leftWidth), abs(rightWidth)));
		}

		//these are the starting values, taken from the left edge
		float invw = pLeft->invw.curr;
		float u = pLeft->u.curr;
		float v = pLeft->v.curr;
		float z = pLeft->z.curr;
		float color[3] = {
			pLeft->color[0].curr,
			pLeft->color[1].curr,
			pLeft->color[2].curr };

		//our dx values are taken from the steps up until the right edge
		float invWidth = 1.0f / width;
		float dinvw_dx = (pRight->invw.curr - invw) * invWidth;
		float du_dx = (pRight->u.curr - u) * invWidth;
		float dv_dx = (pRight->v.curr - v) * invWidth;
		float dz_dx = (pRight->z.curr - z) * invWidth;
		float dc_dx[3] = {
			(pRight->color[0].curr - color[0]) * invWidth,
			(pRight->color[1].curr - color[1]) * invWidth,
			(pRight->color[2].curr - color[2]) * invWidth };

		int adr = (pLeft->Y*framebufferWidth)+XStart;

		//CONSIDER: in case some other math is wrong (shouldve been clipped OK), we might go out of bounds here.
		//better check the Y value.
		if(RENDERER && (pLeft->Y<0 || pLeft->Y>191)) {
			printf("rasterizer rendering at y=%d! oops!\n",pLeft->Y);
			return;
		}
		if(!RENDERER && (pLeft->Y<0 || pLeft->Y>=framebufferHeight)) {
			printf("rasterizer rendering at y=%d! oops!\n",pLeft->Y);
			return;
		}

		int x = XStart;

		if(x<0)
		{
			if(RENDERER && !lineHack)
			{
				printf("rasterizer rendering at x=%d! oops!\n",x);
				return;
			}
			invw += dinvw_dx * -x;
			u += du_dx * -x;
			v += dv_dx * -x;
			z += dz_dx * -x;
			color[0] += dc_dx[0] * -x;
			color[1] += dc_dx[1] * -x;
			color[2] += dc_dx[2] * -x;
			adr += -x;
			width -= -x;
			x = 0;
		}
		if(x+width > (RENDERER?GFX3D_FRAMEBUFFER_WIDTH:framebufferWidth))
		{
			if(RENDERER && !lineHack)
			{
				printf("rasterizer rendering at x=%d! oops!\n",x+width-1);
				return;
			}
			width = (RENDERER?GFX3D_FRAMEBUFFER_WIDTH:framebufferWidth)-x;
		}

		while(width-- > 0)
		{
			pixel(polyAttr, this->_softRender->screenAttributes[adr], this->_softRender->screenColor[adr], color[0], color[1], color[2], u, v, 1.0f/invw, z);
			adr++;
			x++;

			invw += dinvw_dx;
			u += du_dx;
			v += dv_dx;
			z += dz_dx;
			color[0] += dc_dx[0];
			color[1] += dc_dx[1];
			color[2] += dc_dx[2];
		}
	}

	//runs several scanlines, until an edge is finished
	template<bool SLI>
	void runscanlines(const PolygonAttributes &polyAttr, FragmentColor *color, const size_t framebufferWidth, const size_t framebufferHeight, edge_fx_fl *left, edge_fx_fl *right, bool horizontal, bool lineHack)
	{
		//oh lord, hack city for edge drawing

		//do not overstep either of the edges
		int Height = min(left->Height,right->Height);
		bool first=true;

		//HACK: special handling for horizontal line poly
		if (lineHack && left->Height == 0 && right->Height == 0 && left->Y<GFX3D_FRAMEBUFFER_HEIGHT && left->Y>=0)
		{
			bool draw = (!SLI || (left->Y & SLI_MASK) == SLI_VALUE);
			if(draw) drawscanline(polyAttr, framebufferWidth, framebufferHeight, left,right,lineHack);
		}

		while(Height--)
		{
			bool draw = (!SLI || (left->Y & SLI_MASK) == SLI_VALUE);
			if(draw) drawscanline(polyAttr, framebufferWidth, framebufferHeight, left,right,lineHack);
			const int xl = left->X;
			const int xr = right->X;
			const int y = left->Y;
			left->Step();
			right->Step();

			if(!RENDERER && _debug_thisPoly)
			{
				//debug drawing
				bool top = (horizontal&&first);
				bool bottom = (!Height&&horizontal);
				if(Height || top || bottom)
				{
					if(draw)
					{
						int nxl = left->X;
						int nxr = right->X;
						if(top)
						{
							int xs = min(xl,xr);
							int xe = max(xl,xr);
							for (int x = xs; x <= xe; x++)
							{
								int adr = (y*framebufferWidth)+x;
								color[adr].r = 63;
								color[adr].g = 0;
								color[adr].b = 0;
							}
						}
						else if(bottom)
						{
							int xs = min(xl,xr);
							int xe = max(xl,xr);
							for (int x = xs; x <= xe; x++)
							{
								int adr = (y*framebufferWidth)+x;
								color[adr].r = 63;
								color[adr].g = 0;
								color[adr].b = 0;
							}
						}
						else
						{
							int xs = min(xl,nxl);
							int xe = max(xl,nxl);
							for (int x = xs; x <= xe; x++)
							{
								int adr = (y*framebufferWidth)+x;
								color[adr].r = 63;
								color[adr].g = 0;
								color[adr].b = 0;
							}
							xs = min(xr,nxr);
							xe = max(xr,nxr);
							for (int x = xs; x <= xe; x++)
							{
								int adr = (y*framebufferWidth)+x;
								color[adr].r = 63;
								color[adr].g = 0;
								color[adr].b = 0;
							}
						}

					}
				}
				first = false;
			}
		}
	}

	
	//rotates verts counterclockwise
	template<int type>
	INLINE void rot_verts()
	{
		#define ROTSWAP(X) if(type>X) swap(verts[X-1],verts[X]);
		ROTSWAP(1); ROTSWAP(2); ROTSWAP(3); ROTSWAP(4);
		ROTSWAP(5); ROTSWAP(6); ROTSWAP(7); ROTSWAP(8); ROTSWAP(9);
	}

	//rotate verts until vert0.y is minimum, and then vert0.x is minimum in case of ties
	//this is a necessary precondition for our shape engine
	template<int type>
	void sort_verts(bool backwards)
	{
		//if the verts are backwards, reorder them first
		if (backwards)
			for (size_t i = 0; i < type/2; i++)
				swap(verts[i],verts[type-i-1]);

		for (;;)
		{
			//this was the only way we could get this to unroll
			#define CHECKY(X) if(type>X) if(verts[0]->y > verts[X]->y) goto doswap;
			CHECKY(1); CHECKY(2); CHECKY(3); CHECKY(4);
			CHECKY(5); CHECKY(6); CHECKY(7); CHECKY(8); CHECKY(9);
			break;
			
		doswap:
			rot_verts<type>();
		}
		
		while (verts[0]->y == verts[1]->y && verts[0]->x > verts[1]->x)
		{
			rot_verts<type>();
			// hack for VC++ 2010 (bug in compiler optimization?)
			// freeze on 3D
			// TODO: study it
			#ifdef SLEEP_HACK_2011
				Sleep(0); // nop
			#endif
		}
		
	}

	//This function can handle any convex N-gon up to octagons
	//verts must be clockwise.
	//I didnt reference anything for this algorithm but it seems like I've seen it somewhere before.
	//Maybe it is like crow's algorithm
	template<bool SLI>
	void shape_engine(const PolygonAttributes &polyAttr, FragmentColor *color, const size_t framebufferWidth, const size_t framebufferHeight, int type, const bool backwards, bool lineHack)
	{
		bool failure = false;

		switch(type)
		{
			case 3: sort_verts<3>(backwards); break;
			case 4: sort_verts<4>(backwards); break;
			case 5: sort_verts<5>(backwards); break;
			case 6: sort_verts<6>(backwards); break;
			case 7: sort_verts<7>(backwards); break;
			case 8: sort_verts<8>(backwards); break;
			case 9: sort_verts<9>(backwards); break;
			case 10: sort_verts<10>(backwards); break;
			default: printf("skipping type %d\n",type); return;
		}

		//we are going to step around the polygon in both directions starting from vert 0.
		//right edges will be stepped over clockwise and left edges stepped over counterclockwise.
		//these variables track that stepping, but in order to facilitate wrapping we start extra high
		//for the counter we're decrementing.
		int lv = type, rv = 0;

		edge_fx_fl left, right;
		bool step_left = true, step_right = true;
		for (;;)
		{
			//generate new edges if necessary. we must avoid regenerating edges when they are incomplete
			//so that they can be continued on down the shape
			assert(rv != type);
			int _lv = lv==type?0:lv; //make sure that we ask for vert 0 when the variable contains the starting value
			if (step_left) left = edge_fx_fl(_lv,lv-1,(VERT**)&verts, failure);
			if (step_right) right = edge_fx_fl(rv,rv+1,(VERT**)&verts, failure);
			step_left = step_right = false;

			//handle a failure in the edge setup due to nutty polys
			if(failure) 
				return;

			bool horizontal = left.Y == right.Y;
			runscanlines<SLI>(polyAttr, color, framebufferWidth, framebufferHeight, &left, &right, horizontal, lineHack);

			//if we ran out of an edge, step to the next one
			if (right.Height == 0)
			{
				step_right = true;
				rv++;
			} 
			if (left.Height == 0)
			{
				step_left = true;
				lv--;
			}

			//this is our completion condition: when our stepped edges meet in the middle
			if (lv<=rv+1) break;
		}
	}
	
	template<bool SLI>
	FORCEINLINE void mainLoop()
	{
		lastTexKey = NULL;
		
		PolygonAttributes polyAttr;
		u32 lastPolyAttr = 0;
		u32 lastTextureFormat = 0, lastTexturePalette = 0;

		//iterate over polys
		bool first=true;
		for (size_t i = 0; i < this->_softRender->_clippedPolyCount; i++)
		{
			if (!RENDERER) _debug_thisPoly = (i == this->_softRender->_debug_drawClippedUserPoly);
			if (!this->_softRender->polyVisible[i]) continue;
			polynum = i;

			GFX3D_Clipper::TClippedPoly &clippedPoly = this->_softRender->clippedPolys[i];
			POLY *thePoly = clippedPoly.poly;
			int type = clippedPoly.type;

			if (first || lastPolyAttr != thePoly->polyAttr)
			{
				polyAttr = thePoly->getAttributes();
				lastPolyAttr = thePoly->polyAttr;
			}


			if (first || lastTextureFormat != thePoly->texParam || lastTexturePalette != thePoly->texPalette)
			{
				sampler.setup(thePoly->texParam);
				lastTextureFormat = thePoly->texParam;
				lastTexturePalette = thePoly->texPalette;
			}

			first = false;

			lastTexKey = this->_softRender->polyTexKeys[i];
			
			for (int j = 0; j < type; j++)
				this->verts[j] = &clippedPoly.clipVerts[j];
			for (int j = type; j < MAX_CLIPPED_VERTS; j++)
				this->verts[j] = NULL;
			
			shape_engine<SLI>(polyAttr, this->_softRender->screenColor, this->_softRender->_framebufferWidth, this->_softRender->_framebufferHeight, type, !this->_softRender->polyBackfacing[i], (thePoly->vtxFormat & 4) && CommonSettings.GFX3D_LineHack);
		}
	}


}; //rasterizerUnit

#define _MAX_CORES 16
static Task rasterizerUnitTask[_MAX_CORES];
static RasterizerUnit<true> rasterizerUnit[_MAX_CORES];
static RasterizerUnit<false> _HACK_viewer_rasterizerUnit;
static size_t rasterizerCores = 0;
static bool rasterizerUnitTasksInited = false;

static void* execRasterizerUnit(void *arg)
{
	intptr_t which = (intptr_t)arg;
	rasterizerUnit[which].mainLoop<true>();
	return 0;
}

static void* SoftRasterizer_RunCalculateVertices(void *arg)
{
	SoftRasterizerRenderer *softRender = (SoftRasterizerRenderer *)arg;
	softRender->performViewportTransforms<false>();
	softRender->performBackfaceTests();
	softRender->performCoordAdjustment();
	
	return NULL;
}

static void* SoftRasterizer_RunSetupTextures(void *arg)
{
	SoftRasterizerRenderer *softRender = (SoftRasterizerRenderer *)arg;
	softRender->setupTextures();
	
	return NULL;
}

static void* SoftRasterizer_RunUpdateTables(void *arg)
{
	SoftRasterizerRenderer *softRender = (SoftRasterizerRenderer *)arg;
	softRender->UpdateToonTable(softRender->currentRenderState->u16ToonTable);
	softRender->UpdateFogTable(softRender->currentRenderState->fogDensityTable);
	softRender->UpdateEdgeMarkColorTable(softRender->currentRenderState->edgeMarkColorTable);
	
	return NULL;
}

static void* SoftRasterizer_RunClearFramebuffer(void *arg)
{
	SoftRasterizerRenderer *softRender = (SoftRasterizerRenderer *)arg;
	softRender->ClearFramebuffer(*softRender->currentRenderState);
	
	return NULL;
}

void _HACK_Viewer_ExecUnit()
{
	_HACK_viewer_rasterizerUnit.mainLoop<false>();
}

static char SoftRastInit(void)
{
	char result = Default3D_Init();
	if (result == 0)
	{
		return result;
	}
	
	if (_SoftRastRenderer == NULL)
	{
		_SoftRastRenderer = new SoftRasterizerRenderer;
	}
	
	return result;
}

static void SoftRastReset()
{
	_SoftRastRenderer->Reset();
}

static void SoftRastClose()
{
	delete _SoftRastRenderer;
	_SoftRastRenderer = NULL;
}

static void SoftRastRender()
{
	_SoftRastRenderer->Render(gfx3d);
}

static void SoftRastVramReconfigureSignal()
{
	_SoftRastRenderer->VramReconfigureSignal();
}

static void SoftRastRenderFinish()
{
	_SoftRastRenderer->RenderFinish();
}

GPU3DInterface gpu3DRasterize = {
	"SoftRasterizer",
	SoftRastInit,
	SoftRastReset,
	SoftRastClose,
	SoftRastRender,
	SoftRastRenderFinish,
	SoftRastVramReconfigureSignal
};

SoftRasterizerRenderer::SoftRasterizerRenderer()
{
	_debug_drawClippedUserPoly = -1;
	clippedPolys = clipper.clippedPolys = new GFX3D_Clipper::TClippedPoly[POLYLIST_SIZE*2];
	
	_stateSetupNeedsFinish = false;
	_renderGeometryNeedsFinish = false;
	_framebufferWidth = GFX3D_FRAMEBUFFER_WIDTH;
	_framebufferHeight = GFX3D_FRAMEBUFFER_HEIGHT;
	screenAttributes = (FragmentAttributes *)malloc(_framebufferWidth * _framebufferHeight * sizeof(FragmentAttributes));
	screenColor = (FragmentColor *)malloc(_framebufferWidth * _framebufferHeight * sizeof(FragmentColor));
	
	if (!rasterizerUnitTasksInited)
	{
		_HACK_viewer_rasterizerUnit._debug_thisPoly = false;
		_HACK_viewer_rasterizerUnit.SLI_MASK = 1;
		_HACK_viewer_rasterizerUnit.SLI_VALUE = 0;
		
		rasterizerCores = CommonSettings.num_cores;
		
		if (rasterizerCores > _MAX_CORES)
			rasterizerCores = _MAX_CORES;
		
		if (rasterizerCores == 0 || rasterizerCores == 1)
		{
			rasterizerCores = 1;
			rasterizerUnit[0]._debug_thisPoly = false;
			rasterizerUnit[0].SLI_MASK = 0;
			rasterizerUnit[0].SLI_VALUE = 0;
		}
		else
		{
			for (size_t i = 0; i < rasterizerCores; i++)
			{
				rasterizerUnit[i]._debug_thisPoly = false;
				rasterizerUnit[i].SLI_MASK = (rasterizerCores - 1);
				rasterizerUnit[i].SLI_VALUE = i;
				rasterizerUnitTask[i].start(false);
			}
		}
		
		rasterizerUnitTasksInited = true;
	}
	
	InitTables();
	Reset();
	
	printf("SoftRast Initialized with cores=%d\n", (int)rasterizerCores);
}

SoftRasterizerRenderer::~SoftRasterizerRenderer()
{
	if (rasterizerCores > 1)
	{
		for (size_t i = 0; i < rasterizerCores; i++)
		{
			rasterizerUnitTask[i].finish();
			rasterizerUnitTask[i].shutdown();
		}
	}
	
	rasterizerUnitTasksInited = false;
	
	free(screenAttributes);
	free(screenColor);
	
	Default3D_Close();
}

Render3DError SoftRasterizerRenderer::InitTables()
{
	static bool needTableInit = true;
	
	if(needTableInit)
	{
		for (size_t i = 0; i < 64; i++)
		{
			for (size_t j = 0; j < 64; j++)
			{
				modulate_table[i][j] = ((i+1) * (j+1) - 1) >> 6;
				for (size_t a = 0; a < 32; a++)
					decal_table[a][i][j] = ((i*a) + (j*(31-a))) >> 5;
			}
		}
		
		//these tables are used to increment through vert lists without having to do wrapping logic/math
		u8 idx = 0;
		for (u8 i = 3; i <= 8; i++)
		{
			index_start_table[i-3] = idx;
			for (u8 j = 0; j < i; j++)
			{
				u8 a = j;
				u8 b = j+1;
				if (b == i) b = 0;
				index_lookup_table[idx++] = a;
				index_lookup_table[idx++] = b;
			}
		}
		
		needTableInit = false;
	}
	
	return RENDER3DERROR_NOERR;
}

size_t SoftRasterizerRenderer::performClipping(bool hirez, const VERTLIST *vertList, const POLYLIST *polyList, const INDEXLIST *indexList)
{
	//submit all polys to clipper
	clipper.reset();
	for (size_t i = 0; i < polyList->count; i++)
	{
		const POLY &poly = polyList->list[indexList->list[i]];
		const VERT *clipVerts[4] = {
			&vertList->list[poly.vertIndexes[0]],
			&vertList->list[poly.vertIndexes[1]],
			&vertList->list[poly.vertIndexes[2]],
			poly.type==4
			?&vertList->list[poly.vertIndexes[3]]
			:NULL
		};
		
		if(hirez)
			clipper.clipPoly<true>(poly, clipVerts);
		else
			clipper.clipPoly<false>(poly, clipVerts);
	}
	
	return clipper.clippedPolyCounter;
}

template<bool CUSTOM> void SoftRasterizerRenderer::performViewportTransforms()
{
	const float xfactor = (float)this->_framebufferWidth/(float)GFX3D_FRAMEBUFFER_WIDTH;
	const float yfactor = (float)this->_framebufferHeight/(float)GFX3D_FRAMEBUFFER_HEIGHT;
	const float xmax = GFX3D_FRAMEBUFFER_WIDTH*xfactor-(CUSTOM?0.001f:0); //fudge factor to keep from overrunning render buffers
	const float ymax = GFX3D_FRAMEBUFFER_HEIGHT*yfactor-(CUSTOM?0.001f:0);
	
	//viewport transforms
	for (size_t i = 0; i < this->_clippedPolyCount; i++)
	{
		GFX3D_Clipper::TClippedPoly &poly = clippedPolys[i];
		for (size_t j = 0; j < poly.type; j++)
		{
			VERT &vert = poly.clipVerts[j];
			
			//homogeneous divide
			vert.coord[0] = (vert.coord[0]+vert.coord[3]) / (2*vert.coord[3]);
			vert.coord[1] = (vert.coord[1]+vert.coord[3]) / (2*vert.coord[3]);
			vert.coord[2] = (vert.coord[2]+vert.coord[3]) / (2*vert.coord[3]);
			vert.texcoord[0] /= vert.coord[3];
			vert.texcoord[1] /= vert.coord[3];
			
			//CONSIDER: do we need to guarantee that these are in bounds? perhaps not.
			//vert.coord[0] = max(0.0f,min(1.0f,vert.coord[0]));
			//vert.coord[1] = max(0.0f,min(1.0f,vert.coord[1]));
			//vert.coord[2] = max(0.0f,min(1.0f,vert.coord[2]));
			
			//perspective-correct the colors
			vert.fcolor[0] /= vert.coord[3];
			vert.fcolor[1] /= vert.coord[3];
			vert.fcolor[2] /= vert.coord[3];
			
			//viewport transformation
			VIEWPORT viewport;
			viewport.decode(poly.poly->viewport);
			vert.coord[0] *= viewport.width * xfactor;
			vert.coord[0] += viewport.x * xfactor;
			vert.coord[1] *= viewport.height * yfactor;
			vert.coord[1] += viewport.y * yfactor;
			vert.coord[1] = ymax - vert.coord[1];
			
			//well, i guess we need to do this to keep Princess Debut from rendering huge polys.
			//there must be something strange going on
			vert.coord[0] = max(0.0f,min(xmax,vert.coord[0]));
			vert.coord[1] = max(0.0f,min(ymax,vert.coord[1]));
		}
	}
}
//these templates needed to be instantiated manually
template void SoftRasterizerRenderer::performViewportTransforms<true>();
template void SoftRasterizerRenderer::performViewportTransforms<false>();

void SoftRasterizerRenderer::performCoordAdjustment()
{
	for (size_t i = 0; i < this->_clippedPolyCount; i++)
	{
		GFX3D_Clipper::TClippedPoly &clippedPoly = clippedPolys[i];
		int type = clippedPoly.type;
		VERT *verts = &clippedPoly.clipVerts[0];
		
		//here is a hack which needs to be removed.
		//at some point our shape engine needs these to be converted to "fixed point"
		//which is currently just a float
		for (size_t j = 0; j < type; j++)
			for (size_t k = 0; k < 2; k++)
				verts[j].coord[k] = (float)iround(16.0f * verts[j].coord[k]);
	}
}


void SoftRasterizerRenderer::setupTextures()
{
	TexCacheItem *lastTexKey = NULL;
	u32 lastTextureFormat = 0, lastTexturePalette = 0;
	bool needInitTexture = true;
	
	for (size_t i = 0; i < this->_clippedPolyCount; i++)
	{
		GFX3D_Clipper::TClippedPoly &clippedPoly = clippedPolys[i];
		POLY *thePoly = clippedPoly.poly;
		
		//make sure all the textures we'll need are cached
		//(otherwise on a multithreaded system there will be multiple writers--
		//this SHOULD be read-only, although some day the texcache may collect statistics or something
		//and then it won't be safe.
		if (needInitTexture || lastTextureFormat != thePoly->texParam || lastTexturePalette != thePoly->texPalette)
		{
			lastTexKey = TexCache_SetTexture(TexFormat_15bpp, thePoly->texParam, thePoly->texPalette);
			lastTextureFormat = thePoly->texParam;
			lastTexturePalette = thePoly->texPalette;
			needInitTexture = false;
		}
		
		//printf("%08X %d\n",poly->texParam,rasterizerUnit[0].textures.currentNum);
		polyTexKeys[i] = lastTexKey;
	}
}

bool PolygonIsVisible(const PolygonAttributes &polyAttr, const bool backfacing)
{
	//this was added after adding multi-bit stencil buffer
	//it seems that we also need to prevent drawing back faces of shadow polys for rendering
	if (polyAttr.polygonMode == 3 && polyAttr.polygonID != 0) return !backfacing;
	
	//another reasonable possibility is that we should be forcing back faces to draw (mariokart doesnt use them)
	//and then only using a single bit buffer (but a cursory test of this doesnt actually work)
	//
	//this code needs to be here for shadows in wizard of oz to work.
	
	switch (polyAttr.surfaceCullingMode)
	{
		case 0: return false;
		case 1: return backfacing;
		case 2: return !backfacing;
		case 3: return true;
		default: assert(false); return false;
	}
	
	return false;
}

void SoftRasterizerRenderer::performBackfaceTests()
{
	for (size_t i = 0; i < this->_clippedPolyCount; i++)
	{
		GFX3D_Clipper::TClippedPoly &clippedPoly = clippedPolys[i];
		POLY *thePoly = clippedPoly.poly;
		int type = clippedPoly.type;
		VERT* verts = &clippedPoly.clipVerts[0];
		const PolygonAttributes polyAttr = thePoly->getAttributes();
		
		//HACK: backface culling
		//this should be moved to gfx3d, but first we need to redo the way the lists are built
		//because it is too convoluted right now.
		//(must we throw out verts if a poly gets backface culled? if not, then it might be easier)
		
		//an older approach
		//(not good enough for quads and other shapes)
		//float ab[2], ac[2]; Vector2Copy(ab, verts[1].coord); Vector2Copy(ac, verts[2].coord); Vector2Subtract(ab, verts[0].coord);
		//Vector2Subtract(ac, verts[0].coord); float cross = Vector2Cross(ab, ac); polyAttr.backfacing = (cross>0);
		
		//a better approach
		// we have to support somewhat non-convex polygons (see NSMB world map 1st screen).
		// this version should handle those cases better.
		size_t n = type - 1;
		float facing = (verts[0].y + verts[n].y) * (verts[0].x - verts[n].x)
		+ (verts[1].y + verts[0].y) * (verts[1].x - verts[0].x)
		+ (verts[2].y + verts[1].y) * (verts[2].x - verts[1].x);
		for(int j = 2; j < n; j++)
			facing += (verts[j+1].y + verts[j].y) * (verts[j+1].x - verts[j].x);
		
		polyBackfacing[i] = (facing < 0);
		polyVisible[i] = PolygonIsVisible(polyAttr, polyBackfacing[i]);
	}
}

Render3DError SoftRasterizerRenderer::BeginRender(const GFX3D &engine)
{
	if (rasterizerCores > 1)
	{
		// Force all threads to finish before rendering with new data
		for (size_t i = 0; i < rasterizerCores; i++)
		{
			rasterizerUnitTask[i].finish();
		}
	}
	
	// Keep the current render states for later use
	this->currentRenderState = (GFX3D_State *)&engine.renderState;
	
	this->_clippedPolyCount = this->performClipping(CommonSettings.GFX3D_HighResolutionInterpolateColor, engine.vertlist, engine.polylist, &engine.indexlist);
	
	if (rasterizerCores >= 4)
	{
		rasterizerUnitTask[0].execute(&SoftRasterizer_RunCalculateVertices, this);
		rasterizerUnitTask[1].execute(&SoftRasterizer_RunSetupTextures, this);
		rasterizerUnitTask[2].execute(&SoftRasterizer_RunUpdateTables, this);
		rasterizerUnitTask[3].execute(&SoftRasterizer_RunClearFramebuffer, this);
		this->_stateSetupNeedsFinish = true;
	}
	else
	{
		this->performViewportTransforms<false>();
		this->performBackfaceTests();
		this->performCoordAdjustment();
		this->setupTextures();
		this->UpdateToonTable(engine.renderState.u16ToonTable);
		
		if (this->currentRenderState->enableEdgeMarking)
		{
			this->UpdateEdgeMarkColorTable(this->currentRenderState->edgeMarkColorTable);
		}
		
		if (this->currentRenderState->enableFog)
		{
			this->UpdateFogTable(this->currentRenderState->fogDensityTable);
		}
		
		this->ClearFramebuffer(engine.renderState);
		
		this->_stateSetupNeedsFinish = false;
	}
	
	return RENDER3DERROR_NOERR;
}

Render3DError SoftRasterizerRenderer::RenderGeometry(const GFX3D_State &renderState, const VERTLIST *vertList, const POLYLIST *polyList, const INDEXLIST *indexList)
{
	// If multithreaded, allow for states to finish setting up
	if (this->_stateSetupNeedsFinish)
	{
		rasterizerUnitTask[0].finish();
		rasterizerUnitTask[1].finish();
		rasterizerUnitTask[2].finish();
		rasterizerUnitTask[3].finish();
		this->_stateSetupNeedsFinish = false;
	}
	
	// Render the geometry
	if (rasterizerCores > 1)
	{
		for (size_t i = 0; i < rasterizerCores; i++)
		{
			rasterizerUnitTask[i].execute(&execRasterizerUnit, (void *)i);
		}
		
		this->_renderGeometryNeedsFinish = true;
	}
	else
	{
		rasterizerUnit[0].mainLoop<false>();
		this->_renderGeometryNeedsFinish = false;
	}
	
	//	printf("rendered %d of %d polys after backface culling\n",gfx3d.polylist->count-culled,gfx3d.polylist->count);
	TexCache_EvictFrame();
	
	return RENDER3DERROR_NOERR;
}

Render3DError SoftRasterizerRenderer::RenderEdgeMarking(const u16 *colorTable, const bool useAntialias)
{
	// this looks ok although it's still pretty much a hack,
	// it needs to be redone with low-level accuracy at some point,
	// but that should probably wait until the shape renderer is more accurate.
	// a good test case for edge marking is Sonic Rush:
	// - the edges are completely sharp/opaque on the very brief title screen intro,
	// - the level-start intro gets a pseudo-antialiasing effect around the silhouette,
	// - the character edges in-level are clearly transparent, and also show well through shield powerups.
	
	for (size_t i = 0, y = 0; y < this->_framebufferHeight; y++)
	{
		for (size_t x = 0; x < this->_framebufferWidth; x++, i++)
		{
			FragmentAttributes destFragment = screenAttributes[i];
			u8 self = destFragment.opaquePolyID;
			if(edgeMarkDisabled[self>>3]) continue;
			if(destFragment.isTranslucentPoly) continue;
			
			// > is used instead of != to prevent double edges
			// between overlapping polys of different IDs.
			// also note that the edge generally goes on the outside, not the inside, (maybe needs to change later)
			// and that polys with the same edge color can make edges against each other.
			
			FragmentColor edgeColor = this->edgeMarkTable[self>>3];
			
#define PIXOFFSET(dx,dy) ((dx)+(GFX3D_FRAMEBUFFER_WIDTH*(dy)))
#define ISEDGE(dx,dy) ((x+(dx)!=GFX3D_FRAMEBUFFER_WIDTH) && (x+(dx)!=-1) && (y+(dy)!=GFX3D_FRAMEBUFFER_HEIGHT) && (y+(dy)!=-1) && self > screenAttributes[i+PIXOFFSET(dx,dy)].opaquePolyID)
#define DRAWEDGE(dx,dy) alphaBlend(screenColor[i+PIXOFFSET(dx,dy)], edgeColor)
			
			bool upleft    = ISEDGE(-1,-1);
			bool up        = ISEDGE( 0,-1);
			bool upright   = ISEDGE( 1,-1);
			bool left      = ISEDGE(-1, 0);
			bool right     = ISEDGE( 1, 0);
			bool downleft  = ISEDGE(-1, 1);
			bool down      = ISEDGE( 0, 1);
			bool downright = ISEDGE( 1, 1);
			
			if(upleft && upright && downleft && !downright)
				DRAWEDGE(-1,-1);
			if(up && !down)
				DRAWEDGE(0,-1);
			if(upleft && upright && !downleft && downright)
				DRAWEDGE(1,-1);
			if(left && !right)
				DRAWEDGE(-1,0);
			if(right && !left)
				DRAWEDGE(1,0);
			if(upleft && !upright && downleft && downright)
				DRAWEDGE(-1,1);
			if(down && !up)
				DRAWEDGE(0,1);
			if(!upleft && upright && downleft && downright)
				DRAWEDGE(1,1);
			
#undef PIXOFFSET
#undef ISEDGE
#undef DRAWEDGE
			
		}
	}
	
	return RENDER3DERROR_NOERR;
}

Render3DError SoftRasterizerRenderer::UpdateEdgeMarkColorTable(const u16 *edgeMarkColorTable)
{
	//TODO: need to test and find out whether these get grabbed at flush time, or at render time
	//we can do this by rendering a 3d frame and then freezing the system, but only changing the edge mark colors
	for (size_t i = 0; i < 8; i++)
	{
		u16 col = edgeMarkColorTable[i];
		this->edgeMarkTable[i].color = RGB15TO5555(col, (this->currentRenderState->enableAntialiasing) ? 0x0F : 0x1F);
		this->edgeMarkTable[i].r = GFX3D_5TO6(this->edgeMarkTable[i].r);
		this->edgeMarkTable[i].g = GFX3D_5TO6(this->edgeMarkTable[i].g);
		this->edgeMarkTable[i].b = GFX3D_5TO6(this->edgeMarkTable[i].b);
		
		//zero 20-jun-2013 - this doesnt make any sense. at least, it should be related to the 0x8000 bit. if this is undocumented behaviour, lets write about which scenario proves it here, or which scenario is requiring this code.
		//// this seems to be the only thing that selectively disables edge marking
		//edgeMarkDisabled[i] = (col == 0x7FFF);
		this->edgeMarkDisabled[i] = false;
	}
	
	return RENDER3DERROR_NOERR;
}

Render3DError SoftRasterizerRenderer::UpdateFogTable(const u8 *fogDensityTable)
{
#if 0
	//TODO - this might be a little slow;
	//we might need to hash all the variables and only recompute this when something changes
	const int increment = (0x400 >> shift);
	for (u32 i = 0; i < 32768; i++)
	{
		if (i < offset)
		{
			fogTable[i] = densityTable[0];
			continue;
		}
		
		for (int j = 0; j < 32; j++)
		{
			u32 value = offset + increment*(j+1);
			if (i <= value)
			{
				if (j == 0)
				{
					fogTable[i] = densityTable[0];
					goto done;
				}
				else
				{
					fogTable[i] = ((value-i)*(densityTable[j-1]) + (increment-(value-i))*(densityTable[j]))/increment;
					goto done;
				}
			}
		}
		fogTable[i] = (densityTable[31]);
	done: ;
	}
#else
	// this should behave exactly the same as the previous loop,
	// except much faster. (because it's not a 2d loop and isn't so branchy either)
	// maybe it's fast enough to not need to be cached, now.
	const int increment = ((1 << 10) >> this->currentRenderState->fogShift);
	const int incrementDivShift = 10 - this->currentRenderState->fogShift;
	u32 fogOffset = min<u32>(max<u32>(this->currentRenderState->fogOffset, 0), 32768);
	u32 iMin = min<u32>(32768, (( 1 + 1) << incrementDivShift) + fogOffset + 1 - increment);
	u32 iMax = min<u32>(32768, ((32 + 1) << incrementDivShift) + fogOffset + 1 - increment);
	assert(iMin <= iMax);
	
	// If the fog factor is 127, then treat it as 128.
	u8 fogFactor = (fogDensityTable[0] == 127) ? 128 : fogDensityTable[0];
	memset(this->fogTable, fogFactor, iMin);
	
	for(u32 i = iMin; i < iMax; i++)
	{
		int num = (i - fogOffset + (increment-1));
		int j = (num >> incrementDivShift) - 1;
		u32 value = (num & ~(increment-1)) + fogOffset;
		u32 diff = value - i;
		assert(j >= 1 && j < 32);
		fogFactor = ((diff*(fogDensityTable[j-1]) + (increment-diff)*(fogDensityTable[j])) >> incrementDivShift);
		this->fogTable[i] = (fogFactor == 127) ? 128 : fogFactor;
	}
	
	fogFactor = (fogDensityTable[31] == 127) ? 128 : fogDensityTable[31];
	memset(this->fogTable+iMax, fogFactor, 32768-iMax);
#endif
	
	return RENDER3DERROR_NOERR;
}

Render3DError SoftRasterizerRenderer::RenderFog(const u8 *densityTable, const u32 color, const u32 offset, const u8 shift, const bool alphaOnly)
{
	u32 r = GFX3D_5TO6((color)&0x1F);
	u32 g = GFX3D_5TO6((color>>5)&0x1F);
	u32 b = GFX3D_5TO6((color>>10)&0x1F);
	u32 a = (color>>16)&0x1F;
	const size_t framebufferFragmentCount = this->_framebufferWidth * this->_framebufferHeight;
	
	if (!alphaOnly)
	{
		for (size_t i = 0; i < framebufferFragmentCount; i++)
		{
			const FragmentAttributes &destFragment = screenAttributes[i];
			const size_t fogIndex = destFragment.depth >> 9;
			assert(fogIndex < 32768);
			const u8 fog = (destFragment.isFogged) ? this->fogTable[fogIndex] : 0;
			
			FragmentColor &destFragmentColor = screenColor[i];
			destFragmentColor.r = ((128-fog)*destFragmentColor.r + r*fog)>>7;
			destFragmentColor.g = ((128-fog)*destFragmentColor.g + g*fog)>>7;
			destFragmentColor.b = ((128-fog)*destFragmentColor.b + b*fog)>>7;
			destFragmentColor.a = ((128-fog)*destFragmentColor.a + a*fog)>>7;
		}
	}
	else
	{
		for (size_t i = 0; i < framebufferFragmentCount; i++)
		{
			const FragmentAttributes &destFragment = screenAttributes[i];
			const size_t fogIndex = destFragment.depth >> 9;
			assert(fogIndex < 32768);
			const u8 fog = (destFragment.isFogged) ? this->fogTable[fogIndex] : 0;
			
			FragmentColor &destFragmentColor = screenColor[i];
			destFragmentColor.a = ((128-fog)*destFragmentColor.a + a*fog)>>7;
		}
	}
	
	return RENDER3DERROR_NOERR;
}

Render3DError SoftRasterizerRenderer::UpdateToonTable(const u16 *toonTableBuffer)
{
	//convert the toon colors
	for (size_t i = 0; i < 32; i++)
	{
#ifdef WORDS_BIGENDIAN
		u32 u32temp = RGB15TO32_NOALPHA(toonTableBuffer[i]);
		this->toonColor32LUT[i].r = (u32temp >> 2) & 0x3F;
		this->toonColor32LUT[i].g = (u32temp >> 10) & 0x3F;
		this->toonColor32LUT[i].b = (u32temp >> 18) & 0x3F;
#else
		this->toonColor32LUT[i].color = (RGB15TO32_NOALPHA(toonTableBuffer[i])>>2)&0x3F3F3F3F;
#endif
		//printf("%d %d %d %d\n", this->toonColor32LUT[i].r, this->toonColor32LUT[i].g, this->toonColor32LUT[i].b, this->toonColor32LUT[i].a);
	}
	
	return RENDER3DERROR_NOERR;
}

Render3DError SoftRasterizerRenderer::ClearUsingImage(const u16 *__restrict colorBuffer, const u32 *__restrict depthBuffer, const bool *__restrict fogBuffer, const u8 *__restrict polyIDBuffer)
{
	// The clear image buffer is y-flipped, so we need to flip it back to normal here.
	for (size_t y = 0; y < this->_framebufferHeight; y++)
	{
		for (size_t x = 0; x < this->_framebufferWidth; x++)
		{
			size_t ir = x + (y * this->_framebufferWidth);
			size_t iw = x + ((this->_framebufferHeight - 1 - y) * this->_framebufferWidth);
			
			((u32 *)this->screenColor)[iw] = RGB15TO6665(colorBuffer[ir] & 0x7FFF, (colorBuffer[ir] >> 15) * 31);
			this->screenAttributes[iw].isFogged = fogBuffer[ir];
			this->screenAttributes[iw].depth = depthBuffer[ir];
			this->screenAttributes[iw].opaquePolyID = polyIDBuffer[ir];
			this->screenAttributes[iw].translucentPolyID = kUnsetTranslucentPolyID;
			this->screenAttributes[iw].isTranslucentPoly = false;
			this->screenAttributes[iw].stencil = 0;
		}
	}
	
	return RENDER3DERROR_NOERR;
}

Render3DError SoftRasterizerRenderer::ClearUsingValues(const FragmentColor &clearColor, const FragmentAttributes &clearAttributes) const
{
	FragmentColor convertedClearColor;
	convertedClearColor.r = GFX3D_5TO6(clearColor.r);
	convertedClearColor.g = GFX3D_5TO6(clearColor.g);
	convertedClearColor.b = GFX3D_5TO6(clearColor.b);
	convertedClearColor.a = clearColor.a;
	
	for (size_t i = 0; i < (this->_framebufferWidth * this->_framebufferHeight); i++)
	{
		this->screenAttributes[i] = clearAttributes;
		this->screenColor[i] = convertedClearColor;
	}
	
	return RENDER3DERROR_NOERR;
}

Render3DError SoftRasterizerRenderer::Reset()
{
	if (rasterizerCores > 1)
	{
		for (size_t i = 0; i < rasterizerCores; i++)
		{
			rasterizerUnitTask[i].finish();
			rasterizerUnit[i].SetRenderer(this);
		}
	}
	else
	{
		rasterizerUnit[0].SetRenderer(this);
	}
	
	this->_stateSetupNeedsFinish = false;
	this->_renderGeometryNeedsFinish = false;
	Default3D_Reset();
	
	return RENDER3DERROR_NOERR;
}

Render3DError SoftRasterizerRenderer::Render(const GFX3D &engine)
{
	Render3DError error = RENDER3DERROR_NOERR;
	
	error = this->BeginRender(engine);
	if (error != RENDER3DERROR_NOERR)
	{
		return error;
	}
	
	this->RenderGeometry(engine.renderState, engine.vertlist, engine.polylist, &engine.indexlist);
	this->EndRender(engine.frameCtr);
	
	return error;
}

Render3DError SoftRasterizerRenderer::EndRender(const u64 frameCount)
{
	// If we're not multithreaded, then just do the post-processing steps now.
	if (!this->_renderGeometryNeedsFinish)
	{
		if (this->currentRenderState->enableEdgeMarking)
		{
			this->RenderEdgeMarking(this->currentRenderState->edgeMarkColorTable, this->currentRenderState->enableAntialiasing);
		}
		
		if (this->currentRenderState->enableFog)
		{
			this->RenderFog(this->currentRenderState->fogDensityTable, this->currentRenderState->fogColor, this->currentRenderState->fogOffset, this->currentRenderState->fogShift, this->currentRenderState->enableFogAlphaOnly);
		}
		
		memcpy(gfx3d_convertedScreen, this->screenColor, this->_framebufferWidth * this->_framebufferHeight * sizeof(FragmentColor));
	}
	
	return RENDER3DERROR_NOERR;
}

Render3DError SoftRasterizerRenderer::RenderFinish()
{
	if (!this->_renderGeometryNeedsFinish)
	{
		return RENDER3DERROR_NOERR;
	}
	
	this->_renderGeometryNeedsFinish = false;
	
	for (size_t i = 0; i < rasterizerCores; i++)
	{
		rasterizerUnitTask[i].finish();
	}
	
	if (this->currentRenderState->enableEdgeMarking)
	{
		this->RenderEdgeMarking(this->currentRenderState->edgeMarkColorTable, this->currentRenderState->enableAntialiasing);
	}
	
	if (this->currentRenderState->enableFog)
	{
		this->RenderFog(this->currentRenderState->fogDensityTable, this->currentRenderState->fogColor, this->currentRenderState->fogOffset, this->currentRenderState->fogShift, this->currentRenderState->enableFogAlphaOnly);
	}
	
	memcpy(gfx3d_convertedScreen, this->screenColor, this->_framebufferWidth * this->_framebufferHeight * sizeof(FragmentColor));
	
	return RENDER3DERROR_NOERR;
}
