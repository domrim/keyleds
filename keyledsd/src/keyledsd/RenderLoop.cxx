/* Keyleds -- Gaming keyboard tool
 * Copyright (C) 2017 Julien Hartmann, juli1.hartmann@gmail.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <cassert>
#include <cerrno>
#include <exception>
#include <type_traits>
#include "keyledsd/Device.h"
#include "keyledsd/RenderLoop.h"
#include "tools/accelerated.h"
#include "keyleds.h"
#include "logging.h"

static_assert(std::is_pod<keyleds::RGBAColor>::value, "RGBAColor must be a POD type");
static_assert(sizeof(keyleds::RGBAColor) == 4, "RGBAColor must be tightly packed");

LOGGING("render-loop");

using keyleds::RenderTarget;
using keyleds::Renderer;
using keyleds::RenderLoop;

static std::size_t align(std::size_t value, std::size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

/****************************************************************************/

RenderTarget::RenderTarget(const std::vector<std::size_t> & block_sizes)
 : m_colors(nullptr)
{
    m_blocks.reserve(block_sizes.size());

    // Compute required number of colors. Insert padding in between blocks
    // so all blocks are align_colors-aligned.
    std::size_t totalColors = 0;
    for (auto nbColors : block_sizes) {
        totalColors = align(totalColors + nbColors, align_colors);
    }

    if (::posix_memalign(reinterpret_cast<void**>(&m_colors), align_bytes, totalColors * sizeof(m_colors[0])) != 0) {
        throw std::bad_alloc();
    }
    m_nbColors = totalColors;

    totalColors = 0;
    for (auto nbColors : block_sizes) {
        m_blocks.push_back(&m_colors[totalColors]);
        totalColors = align(totalColors + nbColors, align_colors);
    }
}

RenderTarget::RenderTarget(RenderTarget && other) noexcept
 : m_colors(nullptr)
{
    std::swap(m_colors, other.m_colors);
    m_nbColors = other.m_nbColors;
    m_blocks = std::move(other.m_blocks);
}

RenderTarget::~RenderTarget()
{
    free(m_colors);
}

void keyleds::swap(RenderTarget & lhs, RenderTarget & rhs) noexcept
{
    std::swap(lhs.m_colors, rhs.m_colors);
    std::swap(lhs.m_nbColors, rhs.m_nbColors);
    std::swap(lhs.m_blocks, rhs.m_blocks);
}

void keyleds::blend(RenderTarget & lhs, const RenderTarget & rhs)
{
    assert(lhs.size() == rhs.size());
    ::blend(reinterpret_cast<uint8_t*>(lhs.data()),
            reinterpret_cast<const uint8_t*>(rhs.data()), rhs.size());
}

/****************************************************************************/

Renderer::~Renderer() {}

/****************************************************************************/

RenderLoop::RenderLoop(Device & device, renderer_list renderers, unsigned fps)
    : AnimationLoop(fps),
      m_device(device),
      m_renderers(std::move(renderers)),
      m_state(renderTargetFor(device)),
      m_buffer(renderTargetFor(device))
{
    // Ensure no allocation happens in render()
    std::size_t max = 0;
    for (const auto & block : m_device.blocks()) { max = std::max(max, block.keys().size()); }
    m_directives.reserve(max);
}

RenderLoop::~RenderLoop() {}

void RenderLoop::setRenderers(renderer_list renderers)
{
    std::lock_guard<std::mutex> lock(m_mRenderers);
    m_renderers = std::move(renderers);
    DEBUG("enabled ", m_renderers.size(), " renderers for loop ", this);
}

RenderTarget RenderLoop::renderTargetFor(const Device & device)
{
    std::vector<std::size_t> block_sizes;
    block_sizes.reserve(device.blocks().size());
    for (const auto & block : device.blocks()) {
        block_sizes.push_back(block.keys().size());
    }
    return RenderTarget(block_sizes);
}

bool RenderLoop::render(unsigned long nanosec)
{
    // Run all renderers
    bool hasRenderers;
    {
        std::lock_guard<std::mutex> lock(m_mRenderers);
        hasRenderers = !m_renderers.empty();
        for (const auto & renderer : m_renderers) {
            renderer->render(nanosec, m_buffer);
        }
    }

    if (hasRenderers) {
        m_device.flush();   // Ensure another program using the device did not fill
                            // The inbound report queue.

        // Compute diff
        bool hasChanges = false;
        const auto & blocks = m_device.blocks();
        for (size_t bidx = 0; bidx < blocks.size(); ++bidx) {
            const size_t nKeys = blocks[bidx].keys().size();
            m_directives.clear();

            for (size_t idx = 0; idx < nKeys; ++idx) {
                const auto & color = m_buffer.get(bidx, idx);
                if (color != m_state.get(bidx, idx)) {
                    m_directives.push_back({
                        blocks[bidx].keys()[idx], color.red, color.green, color.blue
                    });
                }
            }
            if (!m_directives.empty()) {
                m_device.setColors(blocks[bidx], m_directives.data(), m_directives.size());
                hasChanges = true;
            }
        }

        // Commit color changes
        if (hasChanges) { m_device.commitColors(); }
        swap(m_state, m_buffer);
    }

    return true;
}

void RenderLoop::run()
{
    try {
        getDeviceState(m_state);
    } catch (Device::error & error) {
        ERROR("device error: ", error.what());
        return;
    }

    m_device.setTimeout(0); // disable timeout detection

    try {
        for (;;) {
            try {
                AnimationLoop::run();
                break;
            } catch (Device::error & error) {
                if (!m_device.resync()) { throw; }
            }
        }
    } catch (Device::error & error) {
        if (!((error.code() == KEYLEDS_ERROR_ERRNO && errno == ENODEV) ||
              error.code() == KEYLEDS_ERROR_TIMEDOUT)) {
            ERROR("device error: ", error.what());
        }
    } catch (std::exception & error) {
        ERROR(error.what());
    }
}

void RenderLoop::getDeviceState(RenderTarget & state)
{
    const auto & blocks = m_device.blocks();

    for (size_t block_idx = 0; block_idx < blocks.size(); ++block_idx) {
        auto colors = m_device.getColors(blocks[block_idx]);

        for (size_t idx = 0; idx < colors.size(); ++idx) {
            auto & color = state.get(block_idx, idx);
            color.red = colors[idx].red;
            color.green = colors[idx].green;
            color.blue = colors[idx].blue;
            color.alpha = 255;
        }
    }
}