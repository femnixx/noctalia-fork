#pragma once

#include "shell/panel/panel.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class Button;
class Flex;
class Label;
class Renderer;
class ConfigService;

namespace tabstrip {

// A layer-shell surface that hosts a row of tabs at the top of the screen.
// Only the active tab's content is materialized; switching tabs destroys the
// previous content and builds the new one.
class TabStripPanel : public Panel {
public:
  explicit TabStripPanel(ConfigService* config);
  ~TabStripPanel() override;

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  float preferredWidth() const override;
  float preferredHeight() const override;
  bool hasDecoration() const override { return true; }
  LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::None; }
  PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Floating; }
  bool fillsWidth() const noexcept override { return true; }
  std::string panelScreenPosition() const override { return "top"; }

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;

  void switchTab(std::size_t index);
  void buildTabBar();
  void rebuildTabContent(std::size_t index);

  ConfigService* m_config = nullptr;
  Flex* m_rootLayout = nullptr;
  Flex* m_tabBar = nullptr;
  Flex* m_contentArea = nullptr;
  std::vector<std::unique_ptr<Flex>> m_tabContents;
  std::size_t m_activeTab = 0;
};

} // namespace tabstrip
