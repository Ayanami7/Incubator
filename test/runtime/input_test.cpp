#define TESTING_ENABLED

#include "runtime/platform/input/input.h"

#include <array>
#include <cstddef>
#include <utility>

#include <gtest/gtest.h>

using Incubator::Input::KeyCode;
using Incubator::Input::KeyState;
using Incubator::Input::MouseButton;
using Incubator::Input::fromGlfwKey;
using Incubator::Input::fromGlfwMouseButton;
using Incubator::Input::fromGlfwAction;

class InputTest : public ::testing::Test
{};

template <std::size_t Size>
void expectKeyMappings(const std::array<std::pair<int, KeyCode>, Size>& mappings)
{
    for (const auto& [glfwKey, keyCode] : mappings)
    {
        SCOPED_TRACE(glfwKey);
        EXPECT_EQ(fromGlfwKey(glfwKey), keyCode);
    }
}

TEST_F(InputTest, MapsRepresentativeLetters)
{
    expectKeyMappings(std::array{std::pair{GLFW_KEY_A, KeyCode::A}, std::pair{GLFW_KEY_M, KeyCode::M},
                                 std::pair{GLFW_KEY_Z, KeyCode::Z}});
}

TEST_F(InputTest, MapsRepresentativeNumberKeys)
{
    expectKeyMappings(std::array{std::pair{GLFW_KEY_0, KeyCode::Num0}, std::pair{GLFW_KEY_5, KeyCode::Num5},
                                 std::pair{GLFW_KEY_9, KeyCode::Num9}});
}

TEST_F(InputTest, MapsRepresentativeFunctionKeys)
{
    expectKeyMappings(std::array{std::pair{GLFW_KEY_F1, KeyCode::F1}, std::pair{GLFW_KEY_F7, KeyCode::F7},
                                 std::pair{GLFW_KEY_F12, KeyCode::F12}});
}

TEST_F(InputTest, MapsNavigationKeys)
{
    expectKeyMappings(std::array{std::pair{GLFW_KEY_UP, KeyCode::Up}, std::pair{GLFW_KEY_DOWN, KeyCode::Down},
                                 std::pair{GLFW_KEY_LEFT, KeyCode::Left}, std::pair{GLFW_KEY_RIGHT, KeyCode::Right},
                                 std::pair{GLFW_KEY_HOME, KeyCode::Home}, std::pair{GLFW_KEY_END, KeyCode::End},
                                 std::pair{GLFW_KEY_PAGE_UP, KeyCode::PageUp},
                                 std::pair{GLFW_KEY_PAGE_DOWN, KeyCode::PageDown},
                                 std::pair{GLFW_KEY_INSERT, KeyCode::Insert},
                                 std::pair{GLFW_KEY_DELETE, KeyCode::Delete}});
}

TEST_F(InputTest, MapsActionKeys)
{
    expectKeyMappings(std::array{std::pair{GLFW_KEY_SPACE, KeyCode::Space}, std::pair{GLFW_KEY_ENTER, KeyCode::Enter},
                                 std::pair{GLFW_KEY_ESCAPE, KeyCode::Escape}, std::pair{GLFW_KEY_TAB, KeyCode::Tab},
                                 std::pair{GLFW_KEY_BACKSPACE, KeyCode::Backspace},
                                 std::pair{GLFW_KEY_CAPS_LOCK, KeyCode::CapsLock}});
}

TEST_F(InputTest, MapsModifierKeys)
{
    expectKeyMappings(std::array{std::pair{GLFW_KEY_LEFT_SHIFT, KeyCode::LeftShift},
                                 std::pair{GLFW_KEY_RIGHT_SHIFT, KeyCode::RightShift},
                                 std::pair{GLFW_KEY_LEFT_CONTROL, KeyCode::LeftControl},
                                 std::pair{GLFW_KEY_RIGHT_CONTROL, KeyCode::RightControl},
                                 std::pair{GLFW_KEY_LEFT_ALT, KeyCode::LeftAlt},
                                 std::pair{GLFW_KEY_RIGHT_ALT, KeyCode::RightAlt},
                                 std::pair{GLFW_KEY_LEFT_SUPER, KeyCode::LeftSuper},
                                 std::pair{GLFW_KEY_RIGHT_SUPER, KeyCode::RightSuper}});
}

TEST_F(InputTest, MapsNumpadKeys)
{
    expectKeyMappings(std::array{std::pair{GLFW_KEY_KP_0, KeyCode::Kp0}, std::pair{GLFW_KEY_KP_5, KeyCode::Kp5},
                                 std::pair{GLFW_KEY_KP_9, KeyCode::Kp9},
                                 std::pair{GLFW_KEY_KP_ADD, KeyCode::KpAdd},
                                 std::pair{GLFW_KEY_KP_SUBTRACT, KeyCode::KpSubtract},
                                 std::pair{GLFW_KEY_KP_ENTER, KeyCode::KpEnter},
                                 std::pair{GLFW_KEY_KP_EQUAL, KeyCode::KpEqual}});
}

TEST_F(InputTest, MapsSymbolKeys)
{
    expectKeyMappings(std::array{std::pair{GLFW_KEY_MINUS, KeyCode::Minus},
                                 std::pair{GLFW_KEY_EQUAL, KeyCode::Equal},
                                 std::pair{GLFW_KEY_LEFT_BRACKET, KeyCode::LeftBracket},
                                 std::pair{GLFW_KEY_RIGHT_BRACKET, KeyCode::RightBracket},
                                 std::pair{GLFW_KEY_SEMICOLON, KeyCode::Semicolon},
                                 std::pair{GLFW_KEY_APOSTROPHE, KeyCode::Apostrophe}});
}

TEST_F(InputTest, MapsSystemAndWorldKeys)
{
    expectKeyMappings(std::array{std::pair{GLFW_KEY_SCROLL_LOCK, KeyCode::ScrollLock},
                                 std::pair{GLFW_KEY_NUM_LOCK, KeyCode::NumLock},
                                 std::pair{GLFW_KEY_PRINT_SCREEN, KeyCode::PrintScreen},
                                 std::pair{GLFW_KEY_PAUSE, KeyCode::Pause}, std::pair{GLFW_KEY_MENU, KeyCode::Menu},
                                 std::pair{GLFW_KEY_WORLD_1, KeyCode::World1},
                                 std::pair{GLFW_KEY_WORLD_2, KeyCode::World2}});
}

TEST_F(InputTest, MapsUnknownAndInvalidKeysToUnknown)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_UNKNOWN), KeyCode::Unknown);
    EXPECT_EQ(fromGlfwKey(999999), KeyCode::Unknown);
}

TEST_F(InputTest, MapsMouseButtons)
{
    const std::array mappings = {std::pair{GLFW_MOUSE_BUTTON_1, MouseButton::Left},
                                 std::pair{GLFW_MOUSE_BUTTON_2, MouseButton::Right},
                                 std::pair{GLFW_MOUSE_BUTTON_3, MouseButton::Middle},
                                 std::pair{GLFW_MOUSE_BUTTON_4, MouseButton::X1},
                                 std::pair{GLFW_MOUSE_BUTTON_5, MouseButton::X2}};
    for (const auto& [glfwButton, mouseButton] : mappings)
    {
        SCOPED_TRACE(glfwButton);
        EXPECT_EQ(fromGlfwMouseButton(glfwButton), mouseButton);
    }
}

TEST_F(InputTest, MapsInvalidMouseButtonsToUnknown)
{
    EXPECT_EQ(fromGlfwMouseButton(999), MouseButton::Unknown);
    EXPECT_EQ(fromGlfwMouseButton(-1), MouseButton::Unknown);
}

TEST_F(InputTest, MapsActionsToKeyStates)
{
    EXPECT_EQ(fromGlfwAction(GLFW_RELEASE), KeyState::Release);
    EXPECT_EQ(fromGlfwAction(GLFW_PRESS), KeyState::Press);
    EXPECT_EQ(fromGlfwAction(GLFW_REPEAT), KeyState::Repeat);
}

TEST_F(InputTest, MapsInvalidActionsToRelease)
{
    EXPECT_EQ(fromGlfwAction(12345), KeyState::Release);
    EXPECT_EQ(fromGlfwAction(-1), KeyState::Release);
}

TEST_F(InputTest, DefaultEnumValuesAreZero)
{
    EXPECT_EQ(static_cast<int>(KeyCode::Unknown), 0);
    EXPECT_EQ(static_cast<int>(MouseButton::Unknown), 0);
    EXPECT_EQ(static_cast<int>(KeyState::Release), 0);
}
