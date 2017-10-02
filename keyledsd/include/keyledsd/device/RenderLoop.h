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
#ifndef KEYLEDS_RENDER_LOOP_H_D7E4709F
#define KEYLEDS_RENDER_LOOP_H_D7E4709F

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>
#include "keyledsd/device/Device.h"
#include "keyledsd/colors.h"
#include "tools/AnimationLoop.h"

namespace keyleds {

class EffectPlugin;

/****************************************************************************/

/** Rendering buffer for key colors
 *
 * Holds RGBA color entries for all keys of a device. All key blocks are in the
 * same memory area. Each block is contiguous, but padding keys may be inserted
 * in between blocks so blocks are SSE2-aligned. The buffers is addressed through
 * a 2-tuple containing the block index and key index within block. No ordering
 * is enforce on blocks or keys, but the for_device static method uses the same
 * order that is detected on the device by the keyleds::Device object.
 */
class RenderTarget final
{
    static constexpr std::size_t   align_bytes = 32;
    static constexpr std::size_t   align_colors = align_bytes / sizeof(RGBAColor);
public:
    using value_type = RGBAColor;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = value_type &;
    using const_reference = const value_type &;
    using iterator = value_type *;
    using const_iterator = const value_type *;
public:
                                RenderTarget(size_type numKeys);
                                RenderTarget(RenderTarget &&) noexcept;
    RenderTarget &              operator=(RenderTarget &&) noexcept;
                                ~RenderTarget();

    iterator                    begin() { return &m_colors[0]; }
    const_iterator              begin() const { return &m_colors[0]; }
    const_iterator              cbegin() const { return &m_colors[0]; }
    iterator                    end() { return &m_colors[m_nbColors]; }
    const_iterator              end() const { return &m_colors[m_nbColors]; }
    const_iterator              cend() const { return &m_colors[m_nbColors]; }
    bool                        empty() const { return false; }
    size_type                   size() const noexcept { return m_nbColors; }
    size_type                   max_size() const noexcept { return m_nbColors; }
    value_type *                data() { return m_colors; }
    const value_type *          data() const { return m_colors; }
    reference                   operator[](size_type idx) { return m_colors[idx]; }
    const_reference             operator[](size_type idx) const { return m_colors[idx]; }

private:
    RGBAColor *                 m_colors;       ///< Color buffer. RGBAColor is a POD type
    std::size_t                 m_nbColors;     ///< Number of items in m_colors

    friend void swap(RenderTarget &, RenderTarget &) noexcept;
};

void swap(RenderTarget &, RenderTarget &) noexcept;
void blend(RenderTarget &, const RenderTarget &);

/****************************************************************************/

/** Device render loop
 *
 * An AnimationLoop that runs a set of Renderers and sends the resulting
 * RenderTarget state to a Device. It assumes entire control of the device.
 * That is, no other thread is allowed to call Device's manipulation methods
 * while a RenderLoop for it exists.
 */
class RenderLoop final : public tools::AnimationLoop
{
public:
    using effect_plugin_list = std::vector<EffectPlugin *>;
public:
                    RenderLoop(Device &, unsigned fps);
                    ~RenderLoop() override;

    /// Returns a lock that bars the render loop from using effects while it is held
    /// Holding it is mandatory for modifying any effect or the list itself
    std::unique_lock<std::mutex>    lock();

    /// Effect list accessor. When using it to modify effects, a lock must be held.
    /// The list only holds pointers, which must be valid as long as they remain
    /// in the list. RenderLoop will not destroy them or interact in any way but
    /// calling their render method.
    effect_plugin_list &            effects() { return m_effects; }

    /// Creates a new render target matching the layout of given device
    static RenderTarget renderTargetFor(const Device &);

private:
    bool            render(unsigned long) override;
    void            run() override;

    /// Reads current device led state into the render target
    void            getDeviceState(RenderTarget & state);

private:
    Device &            m_device;               ///< The device to render to
    effect_plugin_list  m_effects;              ///< Current list of effect plugins (unowned)
    std::mutex          m_mEffects;             ///< Controls access to m_effects

    RenderTarget        m_state;                ///< Current state of the device
    RenderTarget        m_buffer;               ///< Buffer to render into, avoids re-creating it
                                                ///  on every render
    std::vector<Device::ColorDirective> m_directives;   ///< Buffer of directives, avoids new/delete on
                                                        ///< every render
};

/****************************************************************************/

};

#endif