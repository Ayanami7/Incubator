#define TESTING_ENABLED

#include "runtime/platform/input/input.h"

#include <gtest/gtest.h>

using Incubator::Input::KeyCode;
using Incubator::Input::KeyState;
using Incubator::Input::MouseButton;
using Incubator::Input::fromGlfwKey;
using Incubator::Input::fromGlfwMouseButton;
using Incubator::Input::fromGlfwAction;

class InputTest : public ::testing::Test
{};

// ========== fromGlfwKey — Letter keys ==========

TEST_F(InputTest, FromGlfwKeyLetterA)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_A), KeyCode::A);
}

TEST_F(InputTest, FromGlfwKeyLetterM)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_M), KeyCode::M);
}

TEST_F(InputTest, FromGlfwKeyLetterZ)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_Z), KeyCode::Z);
}

// ========== fromGlfwKey — Number keys ==========

TEST_F(InputTest, FromGlfwKeyNumber0)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_0), KeyCode::Num0);
}

TEST_F(InputTest, FromGlfwKeyNumber5)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_5), KeyCode::Num5);
}

TEST_F(InputTest, FromGlfwKeyNumber9)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_9), KeyCode::Num9);
}

// ========== fromGlfwKey — Function keys ==========

TEST_F(InputTest, FromGlfwKeyF1)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_F1), KeyCode::F1);
}

TEST_F(InputTest, FromGlfwKeyF7)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_F7), KeyCode::F7);
}

TEST_F(InputTest, FromGlfwKeyF12)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_F12), KeyCode::F12);
}

// ========== fromGlfwKey — Arrow and navigation keys ==========

TEST_F(InputTest, FromGlfwKeyArrowUp)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_UP), KeyCode::Up);
}

TEST_F(InputTest, FromGlfwKeyArrowDown)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_DOWN), KeyCode::Down);
}

TEST_F(InputTest, FromGlfwKeyArrowLeft)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_LEFT), KeyCode::Left);
}

TEST_F(InputTest, FromGlfwKeyArrowRight)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_RIGHT), KeyCode::Right);
}

TEST_F(InputTest, FromGlfwKeyHome)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_HOME), KeyCode::Home);
}

TEST_F(InputTest, FromGlfwKeyEnd)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_END), KeyCode::End);
}

TEST_F(InputTest, FromGlfwKeyPageUp)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_PAGE_UP), KeyCode::PageUp);
}

TEST_F(InputTest, FromGlfwKeyPageDown)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_PAGE_DOWN), KeyCode::PageDown);
}

TEST_F(InputTest, FromGlfwKeyInsert)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_INSERT), KeyCode::Insert);
}

TEST_F(InputTest, FromGlfwKeyDelete)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_DELETE), KeyCode::Delete);
}

// ========== fromGlfwKey — Action keys ==========

TEST_F(InputTest, FromGlfwKeySpace)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_SPACE), KeyCode::Space);
}

TEST_F(InputTest, FromGlfwKeyEnter)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_ENTER), KeyCode::Enter);
}

TEST_F(InputTest, FromGlfwKeyEscape)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_ESCAPE), KeyCode::Escape);
}

TEST_F(InputTest, FromGlfwKeyTab)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_TAB), KeyCode::Tab);
}

TEST_F(InputTest, FromGlfwKeyBackspace)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_BACKSPACE), KeyCode::Backspace);
}

TEST_F(InputTest, FromGlfwKeyCapsLock)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_CAPS_LOCK), KeyCode::CapsLock);
}

// ========== fromGlfwKey — Modifier keys ==========

TEST_F(InputTest, FromGlfwKeyLeftShift)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_LEFT_SHIFT), KeyCode::LeftShift);
}

TEST_F(InputTest, FromGlfwKeyRightShift)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_RIGHT_SHIFT), KeyCode::RightShift);
}

TEST_F(InputTest, FromGlfwKeyLeftControl)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_LEFT_CONTROL), KeyCode::LeftControl);
}

TEST_F(InputTest, FromGlfwKeyRightControl)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_RIGHT_CONTROL), KeyCode::RightControl);
}

TEST_F(InputTest, FromGlfwKeyLeftAlt)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_LEFT_ALT), KeyCode::LeftAlt);
}

TEST_F(InputTest, FromGlfwKeyRightAlt)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_RIGHT_ALT), KeyCode::RightAlt);
}

TEST_F(InputTest, FromGlfwKeyLeftSuper)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_LEFT_SUPER), KeyCode::LeftSuper);
}

TEST_F(InputTest, FromGlfwKeyRightSuper)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_RIGHT_SUPER), KeyCode::RightSuper);
}

// ========== fromGlfwKey — Numpad keys ==========

TEST_F(InputTest, FromGlfwKeyKp0)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_KP_0), KeyCode::Kp0);
}

TEST_F(InputTest, FromGlfwKeyKp5)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_KP_5), KeyCode::Kp5);
}

TEST_F(InputTest, FromGlfwKeyKp9)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_KP_9), KeyCode::Kp9);
}

TEST_F(InputTest, FromGlfwKeyKpAdd)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_KP_ADD), KeyCode::KpAdd);
}

TEST_F(InputTest, FromGlfwKeyKpSubtract)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_KP_SUBTRACT), KeyCode::KpSubtract);
}

TEST_F(InputTest, FromGlfwKeyKpEnter)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_KP_ENTER), KeyCode::KpEnter);
}

TEST_F(InputTest, FromGlfwKeyKpEqual)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_KP_EQUAL), KeyCode::KpEqual);
}

// ========== fromGlfwKey — Symbol keys ==========

TEST_F(InputTest, FromGlfwKeyMinus)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_MINUS), KeyCode::Minus);
}

TEST_F(InputTest, FromGlfwKeyEqual)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_EQUAL), KeyCode::Equal);
}

TEST_F(InputTest, FromGlfwKeyLeftBracket)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_LEFT_BRACKET), KeyCode::LeftBracket);
}

TEST_F(InputTest, FromGlfwKeyRightBracket)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_RIGHT_BRACKET), KeyCode::RightBracket);
}

TEST_F(InputTest, FromGlfwKeySemicolon)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_SEMICOLON), KeyCode::Semicolon);
}

TEST_F(InputTest, FromGlfwKeyApostrophe)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_APOSTROPHE), KeyCode::Apostrophe);
}

// ========== fromGlfwKey — Lock and system keys ==========

TEST_F(InputTest, FromGlfwKeyScrollLock)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_SCROLL_LOCK), KeyCode::ScrollLock);
}

TEST_F(InputTest, FromGlfwKeyNumLock)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_NUM_LOCK), KeyCode::NumLock);
}

TEST_F(InputTest, FromGlfwKeyPrintScreen)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_PRINT_SCREEN), KeyCode::PrintScreen);
}

TEST_F(InputTest, FromGlfwKeyPause)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_PAUSE), KeyCode::Pause);
}

TEST_F(InputTest, FromGlfwKeyMenu)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_MENU), KeyCode::Menu);
}

// ========== fromGlfwKey — Special/World keys ==========

TEST_F(InputTest, FromGlfwKeyWorld1)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_WORLD_1), KeyCode::World1);
}

TEST_F(InputTest, FromGlfwKeyWorld2)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_WORLD_2), KeyCode::World2);
}

// ========== fromGlfwKey — Unknown/Default ==========

TEST_F(InputTest, FromGlfwKeyUnknownReturnsUnknown)
{
    EXPECT_EQ(fromGlfwKey(GLFW_KEY_UNKNOWN), KeyCode::Unknown);
}

TEST_F(InputTest, FromGlfwKeyInvalidReturnsUnknown)
{
    // A value that has no corresponding GLFW_KEY_* constant
    EXPECT_EQ(fromGlfwKey(999999), KeyCode::Unknown);
    EXPECT_EQ(fromGlfwKey(-1), KeyCode::Unknown);
}

// ========== fromGlfwMouseButton tests ==========

TEST_F(InputTest, FromGlfwMouseButtonLeft)
{
    EXPECT_EQ(fromGlfwMouseButton(GLFW_MOUSE_BUTTON_1), MouseButton::Left);
}

TEST_F(InputTest, FromGlfwMouseButtonRight)
{
    EXPECT_EQ(fromGlfwMouseButton(GLFW_MOUSE_BUTTON_2), MouseButton::Right);
}

TEST_F(InputTest, FromGlfwMouseButtonMiddle)
{
    EXPECT_EQ(fromGlfwMouseButton(GLFW_MOUSE_BUTTON_3), MouseButton::Middle);
}

TEST_F(InputTest, FromGlfwMouseButtonX1)
{
    EXPECT_EQ(fromGlfwMouseButton(GLFW_MOUSE_BUTTON_4), MouseButton::X1);
}

TEST_F(InputTest, FromGlfwMouseButtonX2)
{
    EXPECT_EQ(fromGlfwMouseButton(GLFW_MOUSE_BUTTON_5), MouseButton::X2);
}

TEST_F(InputTest, FromGlfwMouseButtonLeftAlias)
{
    // GLFW_MOUSE_BUTTON_LEFT is an alias for GLFW_MOUSE_BUTTON_1
    EXPECT_EQ(fromGlfwMouseButton(GLFW_MOUSE_BUTTON_LEFT), MouseButton::Left);
}

TEST_F(InputTest, FromGlfwMouseButtonRightAlias)
{
    // GLFW_MOUSE_BUTTON_RIGHT is an alias for GLFW_MOUSE_BUTTON_2
    EXPECT_EQ(fromGlfwMouseButton(GLFW_MOUSE_BUTTON_RIGHT), MouseButton::Right);
}

TEST_F(InputTest, FromGlfwMouseButtonMiddleAlias)
{
    // GLFW_MOUSE_BUTTON_MIDDLE is an alias for GLFW_MOUSE_BUTTON_3
    EXPECT_EQ(fromGlfwMouseButton(GLFW_MOUSE_BUTTON_MIDDLE), MouseButton::Middle);
}

TEST_F(InputTest, FromGlfwMouseButtonInvalidReturnsUnknown)
{
    EXPECT_EQ(fromGlfwMouseButton(999), MouseButton::Unknown);
    EXPECT_EQ(fromGlfwMouseButton(-1), MouseButton::Unknown);
}

// ========== fromGlfwAction tests ==========

TEST_F(InputTest, FromGlfwActionRelease)
{
    EXPECT_EQ(fromGlfwAction(GLFW_RELEASE), KeyState::Release);
}

TEST_F(InputTest, FromGlfwActionPress)
{
    EXPECT_EQ(fromGlfwAction(GLFW_PRESS), KeyState::Press);
}

TEST_F(InputTest, FromGlfwActionRepeat)
{
    EXPECT_EQ(fromGlfwAction(GLFW_REPEAT), KeyState::Repeat);
}

TEST_F(InputTest, FromGlfwActionInvalidReturnsRelease)
{
    EXPECT_EQ(fromGlfwAction(12345), KeyState::Release);
    EXPECT_EQ(fromGlfwAction(-1), KeyState::Release);
}

// ========== Sanity checks — enum values are nonzero / distinct ==========

TEST_F(InputTest, KeyCodeUnknownIsZero)
{
    EXPECT_EQ(static_cast<int>(KeyCode::Unknown), 0);
}

TEST_F(InputTest, MouseButtonUnknownIsZero)
{
    EXPECT_EQ(static_cast<int>(MouseButton::Unknown), 0);
}

TEST_F(InputTest, KeyStateReleaseIsZero)
{
    EXPECT_EQ(static_cast<int>(KeyState::Release), 0);
}
