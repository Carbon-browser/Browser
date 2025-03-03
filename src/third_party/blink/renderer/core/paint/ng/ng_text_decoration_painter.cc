// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/paint/ng/ng_text_decoration_painter.h"

#include "third_party/blink/renderer/core/layout/ng/inline/ng_fragment_item.h"
#include "third_party/blink/renderer/core/layout/svg/layout_svg_inline_text.h"
#include "third_party/blink/renderer/core/paint/ng/ng_text_painter.h"
#include "third_party/blink/renderer/core/paint/paint_info.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/platform/graphics/graphics_context_state_saver.h"

namespace blink {

NGTextDecorationPainter::NGTextDecorationPainter(
    NGTextPainter& text_painter,
    const NGFragmentItem& text_item,
    const PaintInfo& paint_info,
    const ComputedStyle& style,
    const TextPaintStyle& text_style,
    const PhysicalRect& decoration_rect,
    NGHighlightPainter::SelectionPaintState* selection)
    : text_painter_(text_painter),
      text_item_(text_item),
      paint_info_(paint_info),
      style_(style),
      text_style_(text_style),
      decoration_rect_(decoration_rect),
      selection_(selection),
      step_(kBegin),
      phase_(kOriginating) {}

NGTextDecorationPainter::~NGTextDecorationPainter() {
  DCHECK(step_ == kBegin);
}

void NGTextDecorationPainter::UpdateDecorationInfo(
    absl::optional<TextDecorationInfo>& result,
    const ComputedStyle& style,
    const TextPaintStyle& text_style) {
  result.reset();

  if (style.TextDecorationsInEffect() == TextDecorationLine::kNone ||
      // Ellipsis should not have text decorations. This is not defined, but
      // 4 impls do this: <https://github.com/w3c/csswg-drafts/issues/6531>
      text_item_.IsEllipsis())
    return;

  absl::optional<AppliedTextDecoration> effective_selection_decoration =
      UNLIKELY(phase_ == kSelection)
          ? selection_->GetSelectionStyle().selection_text_decoration
          : absl::nullopt;

  if (text_item_.Type() == NGFragmentItem::kSvgText &&
      paint_info_.IsRenderingResourceSubtree()) {
    // Need to recompute a scaled font and a scaling factor because they
    // depend on the scaling factor of an element referring to the text.
    float scaling_factor = 1;
    Font scaled_font;
    LayoutSVGInlineText::ComputeNewScaledFontForStyle(
        *text_item_.GetLayoutObject(), scaling_factor, scaled_font);
    DCHECK(scaling_factor);
    // Adjust the origin of the decoration because
    // NGTextPainter::PaintDecorationsExceptLineThrough() will change the
    // scaling of the GraphicsContext.
    LayoutUnit top = decoration_rect_.offset.top;
    // In svg/text/text-decorations-in-scaled-pattern.svg, the size of
    // ScaledFont() is zero, and the top position is unreliable. So we
    // adjust the baseline position, then shift it for scaled_font.
    top +=
        text_item_.ScaledFont().PrimaryFont()->GetFontMetrics().FixedAscent();
    top *= scaling_factor / text_item_.SvgScalingFactor();
    top -= scaled_font.PrimaryFont()->GetFontMetrics().FixedAscent();
    result.emplace(PhysicalOffset(decoration_rect_.offset.left, top),
                   decoration_rect_.Width(), style,
                   text_painter_.InlineContext(),
                   effective_selection_decoration, &scaled_font,
                   MinimumThickness1(false), scaling_factor);
  } else {
    result.emplace(
        decoration_rect_.offset, decoration_rect_.Width(), style,
        text_painter_.InlineContext(), effective_selection_decoration,
        &text_item_.ScaledFont(),
        MinimumThickness1(text_item_.Type() != NGFragmentItem::kSvgText));
  }
}

void NGTextDecorationPainter::Begin(Phase phase) {
  DCHECK(step_ == kBegin);

  phase_ = phase;
  UpdateDecorationInfo(decoration_info_, style_, text_style_);
  clip_rect_.reset();

  if (decoration_info_ && UNLIKELY(selection_)) {
    clip_rect_.emplace(selection_->RectInWritingModeSpace());

    // Whether it’s best to clip to selection rect on both axes or only inline
    // depends on the situation, but the latter can improve the appearance of
    // decorations. For example, we often paint overlines entirely past the
    // top edge of selection rect, and wavy underlines have similar problems.
    //
    // Sadly there’s no way to clip to a rect of infinite height, so for now,
    // let’s clip to selection rect plus its height both above and below. This
    // should be enough to avoid clipping most decorations in the wild.
    //
    // TODO(dazabani@igalia.com): take text-underline-offset and other
    // text-decoration properties into account?
    clip_rect_->set_y(clip_rect_->y() - clip_rect_->height());
    clip_rect_->set_height(3.0 * clip_rect_->height());
  }

  step_ = kExcept;
}

void NGTextDecorationPainter::PaintExceptLineThrough() {
  DCHECK(step_ == kExcept);

  // Clipping the canvas unnecessarily is expensive, so avoid doing it if the
  // only decoration was a ‘line-through’.
  if (decoration_info_ &&
      decoration_info_->HasAnyLine(~TextDecorationLine::kLineThrough)) {
    GraphicsContextStateSaver state_saver(paint_info_.context, false);
    ClipIfNeeded(state_saver);

    text_painter_.PaintDecorationsExceptLineThrough(
        text_item_, paint_info_, style_, text_style_, *decoration_info_,
        ~TextDecorationLine::kNone, decoration_rect_);
  }

  step_ = kOnly;
}

void NGTextDecorationPainter::PaintOnlyLineThrough() {
  DCHECK(step_ == kOnly);

  // Clipping the canvas unnecessarily is expensive, so avoid doing it if there
  // are no ‘line-through’ decorations.
  if (decoration_info_ &&
      decoration_info_->HasAnyLine(TextDecorationLine::kLineThrough)) {
    GraphicsContextStateSaver state_saver(paint_info_.context, false);
    ClipIfNeeded(state_saver);

    text_painter_.PaintDecorationsOnlyLineThrough(
        text_item_, paint_info_, style_, text_style_, *decoration_info_,
        decoration_rect_);
  }

  step_ = kBegin;
}

void NGTextDecorationPainter::ClipIfNeeded(
    GraphicsContextStateSaver& state_saver) {
  DCHECK(step_ != kBegin);

  if (clip_rect_) {
    state_saver.Save();
    if (phase_ == kSelection)
      paint_info_.context.Clip(*clip_rect_);
    else
      paint_info_.context.ClipOut(*clip_rect_);
  }
}

}  // namespace blink
