/*
    Created on: Oct 22, 2019

	Copyright 2019 flyinghead

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
#pragma once
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_ctx.h"
#include "cfg/option.h"
#include <glm/glm.hpp>
#include <utility>
#include <vector>

static inline Rect intersect(const Rect& l, const Rect& r)
{
	Rect rect;
	rect.origin = glm::max(l.origin, r.origin);
	rect.size = glm::max(glm::min(l.bottomRight(), r.bottomRight()) - rect.origin + glm::ivec2(1, 1), glm::ivec2(0, 0));
	return rect;
}

enum class TileClipping {
	Inside,			// Render stuff outside the region
	Off,    		// Always passes
	Outside    		// Render stuff inside the region
};

// Dreamcast:
// +Y is down
// OpenGL:
// +Y is up in clip, NDC and framebuffer coordinates. BUT we invert the Y coordinate in the renderer.
// Vulkan:
// +Y is down in clip, NDC and framebuffer coordinates
// DirectX9:
// +Y is up in clip and NDC coordinates, but down in framebuffer coordinates
// Y must also be flipped for render-to-texture so that the top of the texture comes first
class TransformMatrix
{
public:
	TransformMatrix(bool directx = false)
		: directx(directx)
	{}
	TransformMatrix(const rend_context& renderingContext, int width, int height, bool directx = false)
		: directx(directx)
	{
		CalcMatrices(&renderingContext, width, height);
	}

	const glm::mat4& GetNormalMatrix() const {
		return normalMatrix;
	}
	const glm::mat4& GetViewportMatrix() const {
		return viewportMatrix;
	}

	void CalcMatrices(const rend_context *renderingContext, int width = 0, int height = 0);
	Rect getBaseScissor() const;
	TileClipping getTileClip(u32 val, Rect& clipRect);

private:
	bool isClipped() const;
	void getScissorScaling(float& scale_x, float& scale_y) const;
	Rect intersectTileAndFBScissor() const;

	const rend_context *renderingContext = nullptr;
	const bool directx;

	glm::mat4 normalMatrix;
	glm::mat4 viewportMatrix;
};

void getPvrFramebufferSize(const rend_context& rendCtx, int& width, int& height);
void getScaledFramebufferSize(const rend_context& rendCtx, int& width, int& height);
// The aspect ratio of the rendered image, before Stretch to Fill
float getOutputImageAspectRatio();
float getOutputFramebufferAspectRatio();
void getDCFramebufferReadSize(const FramebufferInfo& info, int& width, int& height);
void getWriteFBToVramParams(const rend_context& ctx, glm::ivec2& scaledSize, Rect& finalClip);

// The aspect ratio of the DC framebuffer image, before Stretch to Fill
inline static float getDCImageAspectRatio() {
	float aspectRatio = config::Rotate90 ? 3.f / 4.f : 4.f / 3.f;
	return aspectRatio * config::ScreenStretching / 100.f;
}

inline static float getDCFramebufferAspectRatio() {
	if (config::StretchToFill)
		return (float)settings.display.width / settings.display.height;
	return getDCImageAspectRatio();
}

void getWindowboxDimensions(int outwidth, int outheight, float renderAR, int& dx, int& dy, bool rotate);

// The image in the output with rend.AspectRatio: in a frame of 4:3 or 16:9, letterboxed, stretched or cropped
struct OutputLayout
{
	// The image's rectangle in the output, in pixels from the top left corner
	float x, y, w, h;
	// The image is drawn in pieces along one axis, horizontal or vertical: positions in the rectangle
	// from 0 (left or top) to 1, with the matching texture coordinates. A crop shows a part of the texture,
	// a barrel squeezes the cut parts on the edges.
	bool vertical;
	std::vector<std::pair<float, float>> pieces;
};
// The layout of an image of aspect ratio imageAR in the output. False with ORIGINAL or a rotated screen:
// the image is shown as before, with getWindowboxDimensions.
bool getOutputLayout(int outwidth, int outheight, float imageAR, OutputLayout& layout);
void getVideoShift(float& x, float& y);
