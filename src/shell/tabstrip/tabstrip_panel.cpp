#include "shell/tabstrip/tabstrip_panel.h"

#include "config/config_service.h"
#include "core/log.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tabstrip {

namespace {

  constexpr Logger kLog("tabstrip");
  constexpr float kTabButtonHeight = 34.0F;
  constexpr float kContentPadding = 16.0F;
  constexpr float kTabBarGap = 0.0F;

  constexpr std::string_view kTabTitles[] = {
    "Home", "Music", "Clipboard", "Alerts", "Shelf", "Notes", "Controls", "Bluetooth",
  };

} // namespace

TabStripPanel::TabStripPanel(ConfigService* config) : m_config(config) {}

TabStripPanel::~TabStripPanel() = default;

void TabStripPanel::create() {
  const float scale = contentScale();

  auto rootLayout = std::make_unique<Flex>();
  rootLayout->setDirection(FlexDirection::Vertical);
  rootLayout->setGap(0.0F);
  m_rootLayout = rootLayout.get();

  // Tab bar row
  auto tabBar = std::make_unique<Flex>();
  tabBar->setDirection(FlexDirection::Horizontal);
  tabBar->setGap(kTabBarGap * scale);
  tabBar->setMinHeight(kTabButtonHeight * scale);
  tabBar->setPadding(kContentPadding * scale, kContentPadding * scale);
  m_tabBar = tabBar.get();

  for (std::size_t i = 0; i < std::size(kTabTitles); ++i) {
    auto button = ui::button({
        .text = std::string(kTabTitles[i]),
        .fontSize = Style::fontSizeBody * scale,
        .glyphSize = 0.0F,
        .variant = (i == 0 ? ButtonVariant::TabActive : ButtonVariant::Tab),
        .surfaceOpacity = panelCardOpacity(),
        .minWidth = 0.0F,
        .minHeight = kTabButtonHeight * scale,
        .paddingV = Style::spaceSm * scale,
        .paddingH = Style::spaceMd * scale,
        .gap = Style::spaceSm * scale,
    });
    button->setOnClick([this, i] { switchTab(i); });
    auto* btnPtr = button.get();
    m_tabBar->addChild(std::move(button));
    m_tabButtons.push_back(btnPtr);
  }

  m_rootLayout->addChild(std::move(tabBar));

  // Content area
  auto contentArea = std::make_unique<Flex>();
  contentArea->setDirection(FlexDirection::Vertical);
  contentArea->setGap(0.0F);
  contentArea->setFillWidth(true);
  contentArea->setFillHeight(true);
  m_contentArea = contentArea.get();
  m_rootLayout->addChild(std::move(contentArea));

  setRoot(std::move(rootLayout));

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  // Build initial tab content
  rebuildTabContent(0);
}

void TabStripPanel::switchTab(std::size_t index) {
  if (index >= std::size(kTabTitles)) {
    return;
  }
  if (index == m_activeTab) {
    return;
  }

  m_activeTab = index;

  // Update tab bar button states
  for (std::size_t i = 0; i < m_tabButtons.size(); ++i) {
    bool isActive = (i == m_activeTab);
    m_tabButtons[i]->setSelected(isActive);
    m_tabButtons[i]->setVariant(isActive ? ButtonVariant::TabActive : ButtonVariant::Tab);
  }

  // Rebuild content for the new tab
  rebuildTabContent(m_activeTab);
}

void TabStripPanel::rebuildTabContent(std::size_t index) {
  if (m_contentArea == nullptr) {
    return;
  }

  m_rootLayout->removeChild(m_contentArea);

  auto content = std::make_unique<Flex>();
  content->setDirection(FlexDirection::Vertical);
  content->setGap(Style::spaceMd * contentScale());
  content->setPadding(kContentPadding * contentScale(), kContentPadding * contentScale());

  // Build content based on active tab
  switch (index) {
    case 0: // Home
      buildHomeTab(*content);
      break;
    case 1: // Music
      buildMusicTab(*content);
      break;
    case 2: // Clipboard
      buildClipboardTab(*content);
      break;
    case 3: // Alerts
      buildAlertsTab(*content);
      break;
    case 4: // Shelf
      buildShelfTab(*content);
      break;
    case 5: // Notes
      buildNotesTab(*content);
      break;
    case 6: // Controls
      buildControlsTab(*content);
      break;
    case 7: // Bluetooth
      buildBluetoothTab(*content);
      break;
    default:
      break;
  }

  m_contentArea = content.get();
  m_rootLayout->addChild(std::move(content));
}

void TabStripPanel::buildTabBar() {
  // Already built in create()
}

void TabStripPanel::buildHomeTab(Flex& container) {
  const float scale = contentScale();

  auto title = ui::label({
      .text = "Home",
      .fontSize = Style::fontSizeTitle * scale,
      .fontWeight = FontWeight::Bold,
      .color = ColorSpec(Color::white()),
  });
  container.addChild(std::move(title));

  auto subtitle = ui::label({
      .text = "No tabs configured yet",
      .fontSize = Style::fontSizeBody * scale,
      .color = ColorSpec(Color::fromHex("#9a9aa0")),
  });
  container.addChild(std::move(subtitle));
}

void TabStripPanel::buildMusicTab(Flex& container) {
  const float scale = contentScale();

  auto title = ui::label({
      .text = "Music",
      .fontSize = Style::fontSizeTitle * scale,
      .fontWeight = FontWeight::Bold,
      .color = ColorSpec(Color::white()),
  });
  container.addChild(std::move(title));

  auto subtitle = ui::label({
      .text = "No music playing",
      .fontSize = Style::fontSizeBody * scale,
      .color = ColorSpec(Color::fromHex("#9a9aa0")),
  });
  container.addChild(std::move(subtitle));
}

void TabStripPanel::buildClipboardTab(Flex& container) {
  const float scale = contentScale();

  auto title = ui::label({
      .text = "Clipboard",
      .fontSize = Style::fontSizeTitle * scale,
      .fontWeight = FontWeight::Bold,
      .color = ColorSpec(Color::white()),
  });
  container.addChild(std::move(title));

  auto subtitle = ui::label({
      .text = "No clipboard items",
      .fontSize = Style::fontSizeBody * scale,
      .color = ColorSpec(Color::fromHex("#9a9aa0")),
  });
  container.addChild(std::move(subtitle));
}

void TabStripPanel::buildAlertsTab(Flex& container) {
  const float scale = contentScale();

  auto title = ui::label({
      .text = "Alerts",
      .fontSize = Style::fontSizeTitle * scale,
      .fontWeight = FontWeight::Bold,
      .color = ColorSpec(Color::white()),
  });
  container.addChild(std::move(title));

  auto subtitle = ui::label({
      .text = "No alerts",
      .fontSize = Style::fontSizeBody * scale,
      .color = ColorSpec(Color::fromHex("#9a9aa0")),
  });
  container.addChild(std::move(subtitle));
}

void TabStripPanel::buildShelfTab(Flex& container) {
  const float scale = contentScale();

  auto title = ui::label({
      .text = "Shelf",
      .fontSize = Style::fontSizeTitle * scale,
      .fontWeight = FontWeight::Bold,
      .color = ColorSpec(Color::white()),
  });
  container.addChild(std::move(title));

  auto subtitle = ui::label({
      .text = "Drag files here to stage",
      .fontSize = Style::fontSizeBody * scale,
      .color = ColorSpec(Color::fromHex("#9a9aa0")),
  });
  container.addChild(std::move(subtitle));
}

void TabStripPanel::buildNotesTab(Flex& container) {
  const float scale = contentScale();

  auto title = ui::label({
      .text = "Notes",
      .fontSize = Style::fontSizeTitle * scale,
      .fontWeight = FontWeight::Bold,
      .color = ColorSpec(Color::white()),
  });
  container.addChild(std::move(title));

  auto subtitle = ui::label({
      .text = "Jot something down...",
      .fontSize = Style::fontSizeBody * scale,
      .color = ColorSpec(Color::fromHex("#9a9aa0")),
  });
  container.addChild(std::move(subtitle));
}

void TabStripPanel::buildControlsTab(Flex& container) {
  const float scale = contentScale();

  auto title = ui::label({
      .text = "Controls",
      .fontSize = Style::fontSizeTitle * scale,
      .fontWeight = FontWeight::Bold,
      .color = ColorSpec(Color::white()),
  });
  container.addChild(std::move(title));

  auto subtitle = ui::label({
      .text = "Audio output, input, and per-app volumes",
      .fontSize = Style::fontSizeBody * scale,
      .color = ColorSpec(Color::fromHex("#9a9aa0")),
  });
  container.addChild(std::move(subtitle));
}

void TabStripPanel::buildBluetoothTab(Flex& container) {
  const float scale = contentScale();

  auto title = ui::label({
      .text = "Bluetooth",
      .fontSize = Style::fontSizeTitle * scale,
      .fontWeight = FontWeight::Bold,
      .color = ColorSpec(Color::white()),
  });
  container.addChild(std::move(title));

  auto subtitle = ui::label({
      .text = "No Bluetooth devices",
      .fontSize = Style::fontSizeBody * scale,
      .color = ColorSpec(Color::fromHex("#9a9aa0")),
  });
  container.addChild(std::move(subtitle));
}

float TabStripPanel::preferredWidth() const {
  return scaled(780.0F);
}

float TabStripPanel::preferredHeight() const {
  return scaled(520.0F);
}

PanelPlacement TabStripPanel::panelPlacement() const noexcept {
  return PanelPlacement::Floating;
}

void TabStripPanel::doLayout(Renderer& renderer, float width, float height) {
  Panel::doLayout(renderer, width, height);
}

void TabStripPanel::doUpdate(Renderer& renderer) {
  Panel::doUpdate(renderer);
}

} // namespace tabstrip
