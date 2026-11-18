#pragma once

#include "shell/bar/widget.h"

class BrightnessService;
class Glyph;
class Label;
class Slider;
struct wl_output;

class BrightnessWidget : public Widget {
public:
  struct Options {
    bool showGlyph = true;
    bool showLabel = true;
    bool showWhenUnavailable = false;
    // Show an inline brightness slider when there is enough horizontal space (e.g. expanded notch).
    bool showSlider = false;
    GlyphPosition glyphPosition = GlyphPosition::Before;
    bool peek = false;
    int peekDelayMs = kBarRevealDelayMs;
  };

  BrightnessWidget(BrightnessService* brightness, wl_output* output, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);
  void syncWidgetVisibility(bool showWidget);
  void onSliderValueChanged(double value);

  BrightnessService* m_brightness = nullptr;
  wl_output* m_output = nullptr;
  bool m_showGlyph = true;
  bool m_showLabel = true;
  bool m_showSlider = false;
  bool m_showWhenUnavailable = false;
  GlyphPosition m_glyphPosition = GlyphPosition::Before;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  Slider* m_slider = nullptr;
  std::string m_lastDisplayId;
  bool m_haveLastState = false;
  bool m_lastAvailable = false;
  float m_lastBrightness = -1.0F;
  bool m_isVertical = false;
  bool m_lastVertical = false;
};
