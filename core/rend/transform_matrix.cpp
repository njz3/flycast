/*
	Copyright 2026 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#define GLM_FORCE_SWIZZLE 1
#include "transform_matrix.h"
#include "stdclass.h"
#include <glm/gtx/transform.hpp>
#include <algorithm>
#include <cmath>

static void getTAViewport(const rend_context& rendCtx, int& width, int& height) {
	width = rendCtx.globClip.x;
	height = rendCtx.globClip.y;
}

static void makePowerOf2Size(int width, int height, int& wpo2, int& hpo2)
{
	wpo2 = 8;
	hpo2 = 8;
	for (int v = 0; v < 8; v++)
	{
		if (wpo2 < width)
			wpo2 *= 2;
		if (hpo2 < height)
			hpo2 *= 2;
	}
}

void getPvrFramebufferSize(const rend_context& rendCtx, int& width, int& height)
{
	if (!rendCtx.isRTT)
	{
		// Render to screen
		getTAViewport(rendCtx, width, height);
		if (!config::EmulateFramebuffer)
		{
			int maxHeight = FB_R_CTRL.vclk_div == 0 && SPG_CONTROL.interlace == 0 ? 240 : 480;
			// we ignore vscalefactor when interlaced because it's used for Stretched PAL (factor in ]1, 1.3]),
			// or Flicker-free interlace mode B (factor [0.5, 0.6]), which are only emulated in Full FB emu.
			// In 240p, some games (Cho - Hatsumei Boy Kanipan) use a 1/2 factor from rendering at 480 to a 240-line framebuffer.
			// So we render at 480 in that case.
			if (rendCtx.scaler_ctl.vscalefactor != 0
					&& (rendCtx.scaler_ctl.vscalefactor > 1025 || rendCtx.scaler_ctl.vscalefactor < 1024)
					&& SPG_CONTROL.interlace == 0)
				maxHeight /= 1024.f / rendCtx.scaler_ctl.vscalefactor;
			if (FB_R_CTRL.fb_line_double)
				maxHeight /= 2;
			height = std::min(maxHeight, height);
		}
	}
	else
	{
		// Render to texture
		if (config::RenderToTextureBuffer) {
			width = rendCtx.tileClip.bottomRight().x + 1;
			height = rendCtx.tileClip.bottomRight().y + 1;
		}
		else {
			Rect clip = intersect(rendCtx.tileClip, rendCtx.fbClip);
			makePowerOf2Size(clip.bottomRight().x + 1, clip.bottomRight().y + 1, width, height);
		}
	}
}

bool TransformMatrix::isClipped() const
{
	int width, height;
	getTAViewport(*renderingContext, width, height);
	Rect clip = intersectTileAndFBScissor();
	return clip.origin.x > 0
			|| clip.bottomRight().x < width - 1;
}

void TransformMatrix::CalcMatrices(const rend_context *renderingContext, int width, int height)
{
	const int flipY = directx ? -1 : 1;

	glm::vec2 renderViewport = { width == 0 ? settings.display.width : width, height == 0 ? settings.display.height : height };
	glm::vec2 dcViewport;
	this->renderingContext = renderingContext;

	int w, h;
	getPvrFramebufferSize(*renderingContext, w, h);
	dcViewport.x = (float)w;
	dcViewport.y = (float)h;

	if (renderingContext->isRTT) {
		normalMatrix = glm::translate(glm::vec3(-1, -flipY, 0))
			* glm::scale(glm::vec3(2.0f / dcViewport.x, 2.0f / dcViewport.y * flipY, 1.f));
	}
	else
	{
		float widescreenShift;
		if (config::Widescreen && !config::Rotate90 && !config::EmulateFramebuffer)
		{
			widescreenShift = 1.f - dcViewport.x / dcViewport.y * renderViewport.y / renderViewport.x;
			if (config::SuperWidescreen)
				dcViewport.x *= (float)settings.display.width / settings.display.height / 4.f * 3.f;
			else
				dcViewport.x *= 4.f / 3.f;
		}
		else {
			widescreenShift = 0;
		}
		glm::mat4 trans = glm::translate(glm::vec3(-1 + widescreenShift, -flipY, 0));
		float x_coef = 2.0f / dcViewport.x;
		float y_coef = 2.0f / dcViewport.y * flipY;
		normalMatrix = trans * glm::scale(glm::vec3(x_coef, y_coef, 1.f));
	}
	normalMatrix = glm::scale(glm::vec3(1, 1, 1 / config::ExtraDepthScale)) * normalMatrix;

	glm::mat4 vp_trans = glm::translate(glm::vec3(1, flipY, 0));
	vp_trans = glm::scale(glm::vec3(renderViewport.x / 2, renderViewport.y / 2 * flipY, 1.f))
		* vp_trans;
	viewportMatrix = vp_trans * normalMatrix;
}

Rect TransformMatrix::intersectTileAndFBScissor() const
{
	// Framebuffer clipping is applied after scaling (SCALER_CTL).
	// Since we want to apply it before, we need to "unscale" the clipping rectangle.
	glm::ivec2 botRight = renderingContext->fbClip.origin + renderingContext->fbClip.size;
	float xscale, yscale;
	getScissorScaling(xscale, yscale);
	Rect rect;
	rect.origin = glm::max(glm::ivec2(std::round(renderingContext->fbClip.origin.x * xscale), std::round(renderingContext->fbClip.origin.y * yscale)),
			renderingContext->tileClip.origin);
	botRight = glm::min(glm::ivec2(std::round(botRight.x * xscale), std::round(botRight.y * yscale)),
			renderingContext->tileClip.origin + renderingContext->tileClip.size);
	rect.size = glm::max(botRight - rect.origin, glm::ivec2(0, 0));
	return rect;
}

Rect TransformMatrix::getBaseScissor() const
{
	const bool widescreen = !renderingContext->isRTT && config::Widescreen
			&& !isClipped() && !config::Rotate90 && !config::EmulateFramebuffer;
	if (widescreen)
	{
		return Rect{
			glm::ivec2(0, 0),
			glm::ivec2(renderingContext->framebufferWidth, renderingContext->framebufferHeight)
		};
	}

	glm::vec4 origin;
	glm::vec4 size;
	if ((!renderingContext->isRTT && config::EmulateFramebuffer)
			|| (renderingContext->isRTT && config::RenderToTextureBuffer))
	{
		// Region tile clipping only
		origin = glm::vec4(renderingContext->tileClip.origin, 0, 1);
		size = glm::vec4(renderingContext->tileClip.size, 0, 0);
	}
	else
	{
		Rect rect = intersectTileAndFBScissor();
		origin = glm::vec4(rect.origin, 0, 1);
		size = glm::vec4(rect.size, 0, 0);
	}
	origin = viewportMatrix * origin;
	size = viewportMatrix * size;

	if (size.x < 0) {
		origin.x += size.x;
		size.x = -size.x;
	}
	if (size.y < 0) {
		origin.y += size.y;
		size.y = -size.y;
	}

	return Rect{
		glm::ivec2(glm::max(glm::round(origin.xy()), glm::vec2(0, 0))),
		glm::ivec2(glm::max(glm::round(size.xy()), glm::vec2(0, 0)))
	};
}

void TransformMatrix::getScissorScaling(float& scale_x, float& scale_y) const
{
	scale_x = 1.f;
	scale_y = 1.f;

	if ((!renderingContext->isRTT && !config::EmulateFramebuffer)
			|| (renderingContext->isRTT && !config::RenderToTextureBuffer))
	{
		if (renderingContext->scaler_ctl.vscalefactor < 1024 || renderingContext->scaler_ctl.vscalefactor > 1025)
			scale_y *= renderingContext->scaler_ctl.vscalefactor / 1024.f;
		if (renderingContext->scaler_ctl.hscale == 1)
			scale_x *= 2.f;
	}
}

TileClipping TransformMatrix::getTileClip(u32 val, Rect& clipRect)
{
	if (!config::Clipping)
		return TileClipping::Off;

	u32 clipmode = val >> 28;
	if (clipmode < 2)
		return TileClipping::Off;	//always passes

	TileClipping tileClippingMode;
	if (clipmode & 1)
		tileClippingMode = TileClipping::Inside;   //render stuff outside the region
	else
		tileClippingMode = TileClipping::Outside;  //render stuff inside the region

	float csx = (float)(val & 63);
	float cex = (float)((val >> 6) & 63);
	float csy = (float)((val >> 12) & 31);
	float cey = (float)((val >> 17) & 31);
	csx = csx * 32;
	cex = (cex + 1) * 32;
	csy = csy * 32;
	cey = (cey + 1) * 32;

	if (csx == 0 && csy == 0 && cex >= renderingContext->globClip.x && cey >= renderingContext->globClip.y)
		return TileClipping::Off;

	glm::vec4 clip_start(csx, csy, 0, 1);
	glm::vec4 clip_end(cex, cey, 0, 1);
	clip_start = viewportMatrix * clip_start;
	clip_end = viewportMatrix * clip_end;
	clipRect = { clip_start.xy(), clip_end.xy() - clip_start.xy() };
	clipRect.size.x++;
	clipRect.size.y = std::abs(clipRect.size.y) + 1;
	if (tileClippingMode == TileClipping::Outside)
	{
		Rect base = getBaseScissor();
		clipRect = intersect(clipRect, base);
	}
	csx = clip_start[0];
	csy = clip_start[1];
	cey = clip_end[1];
	cex = clip_end[0];
	clipRect = {
		glm::ivec2(std::max(0, (int)std::round(csx)), std::max(0, (int)std::round(std::min(csy, cey)))),
		glm::ivec2(std::max(0, (int)std::round(cex - csx)), std::max(0, (int)std::round(std::abs(cey - csy))))
	};
	return tileClippingMode;
}

void getScaledFramebufferSize(const rend_context& rendCtx, int& width, int& height)
{
	getPvrFramebufferSize(rendCtx, width, height);
	if (!rendCtx.isRTT)
	{
		// Render to screen
		if (!config::EmulateFramebuffer)
		{
			float upscaling = config::RenderResolution / 480.f;
			float w = width * upscaling;
			float h = height * upscaling;
			if (config::Widescreen && !config::Rotate90)
			{
				if (config::SuperWidescreen)
					w *= (float)settings.display.width / settings.display.height / 4.f * 3.f;
				else
					w *= 4.f / 3.f;
			}
			if (!config::Rotate90)
				w = std::round(w / 2.f) * 2.f;
			h = std::round(h);
			width = w;
			height = h;
		}
	}
	else
	{
		// Render to texture
		if (!config::RenderToTextureBuffer)
		{
			if (!config::EmulateFramebuffer)
			{
				float upscaling = config::RenderResolution / 480.f;
				float w = width * upscaling;
				float h = height * upscaling;
				width = std::round(w);
				height = std::round(h);
			}
		}
		else
		{
			// We currently ignore vert and horiz scaling so we need to increase the framebuffer size accordingly
			if (rendCtx.scaler_ctl.vscalefactor < 1024 || rendCtx.scaler_ctl.vscalefactor > 1025)
				height = height * 1024 / rendCtx.scaler_ctl.vscalefactor;
			if (rendCtx.scaler_ctl.hscale == 1)
				width /= 2;
		}
	}
}

float getOutputImageAspectRatio()
{
	float aspectRatio;
	if (config::Rotate90)
	{
		aspectRatio = 3.f / 4.f;
	}
	else
	{
		if (config::Widescreen && !config::EmulateFramebuffer)
		{
			if (config::SuperWidescreen)
				aspectRatio = (float)settings.display.width / settings.display.height;
			else
				aspectRatio = 16.f / 9.f;
		}
		else
		{
			aspectRatio = 4.f / 3.f;
		}
	}
	return aspectRatio * config::ScreenStretching / 100.f;
}

float getOutputFramebufferAspectRatio()
{
	if (config::StretchToFill)
		return (float)settings.display.width / settings.display.height;

	return getOutputImageAspectRatio();
}

// The length of image shown from the inner border of a barrel's edge to x, in an edge of width d that shows
// a length l of image on a cylinder of radius r. Linear when the edge is long enough.
static float barrelArc(float l, float d, float r, float x)
{
	if (l <= d)
		return l * x / d;
	constexpr float HalfPi = 1.57079633f;
	const float t = std::min(l / r, HalfPi);
	return l * std::asin(std::sin(t) * x / d) / t;
}

// The pieces of an image cropped along an axis, the frame keeping the part k of it.
// In frame units, the frame is 1 long and the image 1 / k.
static std::vector<std::pair<float, float>> getCropPieces(float k)
{
	const float imageLength = 1.f / k;
	// The start of the part kept, from the left or top
	const float start = (imageLength - 1.f) * (std::clamp((int)config::CropPosition, -100, 100) + 100) / 200.f;
	// The width of each edge of the barrel. Its middle shows the middle of the part kept at the crop's scale.
	const float edge = config::CropBarrel ? (1.f - std::clamp((int)config::BarrelCenter, 0, 100) / 100.f) / 2.f : 0.f;
	if (edge <= 0.f)
		return { { 0.f, start * k }, { 1.f, (start + 1.f) * k } };

	// Each edge shows the rest of the image on its side, the cut part included
	const float radius = std::max((int)config::BarrelRadius, 1) / 100.f;
	const float before = start + edge;
	const float after = imageLength - start - 1.f + edge;
	constexpr int Steps = 64;
	std::vector<std::pair<float, float>> pieces;
	for (int i = Steps; i >= 0; i--)
	{
		const float x = edge * i / Steps;
		pieces.emplace_back(edge - x, (before - barrelArc(before, edge, radius, x)) * k);
	}
	for (int i = 0; i <= Steps; i++)
	{
		const float x = edge * i / Steps;
		pieces.emplace_back(1.f - edge + x, (start + 1.f - edge + barrelArc(after, edge, radius, x)) * k);
	}
	return pieces;
}

bool getOutputLayout(int outwidth, int outheight, float imageAR, OutputLayout& layout)
{
	enum class Fit { Letterbox, Stretch, Crop } fit;
	float frameAR;
	std::string mode = config::AspectRatio;
	string_toupper(mode);
	if (mode == "4:3" || mode == "4:3_LETTERBOX")
		fit = Fit::Letterbox, frameAR = 4.f / 3.f;
	else if (mode == "16:9")
		fit = Fit::Letterbox, frameAR = 16.f / 9.f;
	else if (mode == "4:3_STRETCH")
		fit = Fit::Stretch, frameAR = 4.f / 3.f;
	else if (mode == "16:9_STRETCH")
		fit = Fit::Stretch, frameAR = 16.f / 9.f;
	else if (mode == "4:3_CROP")
		fit = Fit::Crop, frameAR = 4.f / 3.f;
	else if (mode == "16:9_CROP")
		fit = Fit::Crop, frameAR = 16.f / 9.f;
	else
		return false;
	if (config::Rotate90 || outwidth <= 0 || outheight <= 0 || imageAR <= 0.f)
		return false;

	// The frame: the largest of its shape in the output, or the whole output with Stretch to Fill
	layout.x = layout.y = 0.f;
	layout.w = (float)outwidth;
	layout.h = (float)outheight;
	if (!config::StretchToFill)
	{
		if (frameAR > layout.w / layout.h)
		{
			layout.h = layout.w / frameAR;
			layout.y = (outheight - layout.h) / 2.f;
		}
		else
		{
			layout.w = layout.h * frameAR;
			layout.x = (outwidth - layout.w) / 2.f;
		}
	}
	// The image in the frame, laid out for the frame's shape: a stretched frame stretches it too
	layout.vertical = false;
	layout.pieces = { { 0.f, 0.f }, { 1.f, 1.f } };
	switch (fit)
	{
	case Fit::Stretch:
		break;
	case Fit::Letterbox:
		if (imageAR > frameAR)
		{
			const float h = layout.h * frameAR / imageAR;
			layout.y += (layout.h - h) / 2.f;
			layout.h = h;
		}
		else
		{
			const float w = layout.w * imageAR / frameAR;
			layout.x += (layout.w - w) / 2.f;
			layout.w = w;
		}
		break;
	case Fit::Crop:
		layout.vertical = imageAR < frameAR;
		layout.pieces = getCropPieces(layout.vertical ? imageAR / frameAR : frameAR / imageAR);
		break;
	}
	return true;
}

void getDCFramebufferReadSize(const FramebufferInfo& info, int& width, int& height)
{
	width = (info.fb_r_size.fb_x_size + 1) * 2;     // in 16-bit words
	height = info.fb_r_size.fb_y_size + 1;
	int modulus = (info.fb_r_size.fb_modulus - 1) * 2;

	int bpp;
	switch (info.fb_r_ctrl.fb_depth)
	{
		case fbde_0555:
		case fbde_565:
			bpp = 2;
			break;
		case fbde_888:
			bpp = 3;
			width = (width * 2) / 3;		// in pixels
			modulus = (modulus * 2) / 3;	// in pixels
			break;
		case fbde_C888:
			bpp = 4;
			width /= 2;             // in pixels
			modulus /= 2;           // in pixels
			break;
		default:
			bpp = 2;
			break;
	}

	if (info.spg_control.interlace && width == modulus && info.fb_r_sof2 == info.fb_r_sof1 + width * bpp)
		// Typical case alternating even and odd lines -> take the whole buffer at once
		height *= 2;
}

void getWindowboxDimensions(int outwidth, int outheight, float renderAR, int& dx, int& dy, bool rotate)
{
	if (outwidth == 0 || outheight == 0 || renderAR == 0.f) {
		dx = dy = 0;
		return;
	}
	if (config::IntegerScale)
	{
		int fbh = config::RenderResolution;
		if (fbh == 0) {
			dx = dy = 0;
			return;
		}
		int fbw = (int)((rotate ? 1 / renderAR : renderAR) * fbh);
		if (rotate)
			std::swap(fbw, fbh);

		int scale = std::min(outwidth / fbw, outheight / fbh);
		if (scale == 0) {
			scale = std::max(fbw / outwidth, fbh / outheight) + 1;
			dx = (outwidth - fbw / scale) / 2;
			dy = (outheight - fbh / scale) / 2;
		}
		else {
			dx = (outwidth - fbw * scale) / 2;
			dy = (outheight - fbh * scale) / 2;
		}
	}
	else
	{
		float screenAR = (float)outwidth / outheight;
		if (renderAR > screenAR)
			dy = (int)roundf(outheight * (1 - screenAR / renderAR) / 2.f);
		else
			dx = (int)roundf(outwidth * (1 - renderAR / screenAR) / 2.f);
	}
}

void getVideoShift(float& x, float& y)
{
	const bool vga = FB_R_CTRL.vclk_div == 1;
	switch (SPG_LOAD.hcount)
	{
		case 857: // NTSC, VGA
			x = (int)VO_STARTX.HStart - (vga ? 0xa8 : 0xa4);
			break;
		case 863: // PAL
			x = (int)VO_STARTX.HStart - 0xae;
			break;
		case 851: // Naomi
		case 850: // meltyb
			x = (int)VO_STARTX.HStart - 0xa5; // a0 for 15 kHz
			break;
		default:
			x = 0;
			INFO_LOG(RENDERER, "unknown video mode: hcount %d", SPG_LOAD.hcount);
			break;
	}
	switch (SPG_LOAD.vcount)
	{
		case 524: // NTSC, VGA
			y = (int)VO_STARTY.VStart_field1 - (vga ? 0x28 : 0x12);
			break;
		case 262: // NTSC 240p
			y = (int)VO_STARTY.VStart_field1 - 0x11;
			break;
		case 624: // PAL
			y = (int)VO_STARTY.VStart_field1 - 0x2d;
			break;
		case 312: // PAL 240p
			y = (int)VO_STARTY.VStart_field1 - 0x2e;
			break;
		case 529: // Naomi 31 kHz
		case 528: // meltyb
			y = (int)VO_STARTY.VStart_field1 - 0x24;
			break;
		case 536: // Naomi 15 kHz 480i
		case 268: // Naomi 15 kHz 240p
			y = (int)VO_STARTY.VStart_field1 - 0x17; // 16 for 240p
			break;
		default:
			y = 0;
			INFO_LOG(RENDERER, "unknown video mode: vcount %d", SPG_LOAD.vcount);
			break;
	}
	if (!config::EmulateFramebuffer)
	{
		x *= config::RenderResolution / 480.f;
		y *= config::RenderResolution / 480.f;
	}
	if (config::StretchToFill)
	{
		float aspectRatio = config::Rotate90 ? 3.f / 4.f : 4.f / 3.f;

		if (!config::Rotate90 && config::Widescreen && !config::EmulateFramebuffer)
			aspectRatio = config::SuperWidescreen
					? (float)settings.display.width / settings.display.height
					: 16.f / 9.f;

		x *= ((float)settings.display.width / settings.display.height) / aspectRatio;
	}
	else
	{
		x *= config::ScreenStretching / 100.f;
	}
}

void getWriteFBToVramParams(const rend_context& ctx, glm::ivec2& scaledSize, Rect& finalClip)
{
	glm::vec2 scale;
	scale.x = ctx.scaler_ctl.hscale == 1 ? 0.5f : 1.f;
	if (ctx.scaler_ctl.vscalefactor != 0
			&& ctx.scaler_ctl.vscalefactor != 1024 && ctx.scaler_ctl.vscalefactor != 1025)
		scale.y = 1024.f / ctx.scaler_ctl.vscalefactor;
	else
		scale.y = 1;

	scaledSize.x = ctx.globClip.x * scale.x;
	scaledSize.y = ctx.globClip.y * scale.y;
	finalClip.origin = glm::vec2(ctx.tileClip.origin) * scale;
	finalClip.size = glm::vec2(ctx.tileClip.size) * scale;
	finalClip = intersect(finalClip, ctx.fbClip);
}
