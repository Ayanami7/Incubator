#include "runtime/platform/input/input_state.h"

#include <gtest/gtest.h>

namespace
{
    using namespace Incubator::Input;

    TEST(InputStateTest, FrameClearsOnlyTransientStateAndQueriesDoNotConsume)
    {
        InputState input;
        EXPECT_TRUE(input.focused());
        EXPECT_FALSE(input.key(KeyCode::F).down);
        input.onKey(KeyCode::F, KeyState::Press);
        input.onMouseButton(MouseButton::Right, KeyState::Press);
        EXPECT_TRUE(input.key(KeyCode::F).pressed);
        EXPECT_TRUE(input.key(KeyCode::F).pressed);
        input.beginFrame();
        EXPECT_TRUE(input.key(KeyCode::F).down);
        EXPECT_FALSE(input.key(KeyCode::F).pressed);
        EXPECT_TRUE(input.mouseButton(MouseButton::Right).down);
        EXPECT_FALSE(input.mouseButton(MouseButton::Right).pressed);
        input.onKey(KeyCode::F, KeyState::Repeat);
        input.onKey(KeyCode::F, KeyState::Press);
        EXPECT_FALSE(input.key(KeyCode::F).pressed);
        input.onKey(KeyCode::F, KeyState::Release);
        input.onMouseButton(MouseButton::Right, KeyState::Release);
        EXPECT_TRUE(input.key(KeyCode::F).released);
        EXPECT_TRUE(input.mouseButton(MouseButton::Right).released);
        EXPECT_FALSE(input.key(KeyCode::F).down);
        input.beginFrame();
        EXPECT_FALSE(input.key(KeyCode::F).released);
        EXPECT_FALSE(input.mouseButton(MouseButton::Right).released);
    }

    TEST(InputStateTest, SameRoundTransitionsPreserveBothEdgesAndFinalDown)
    {
        InputState input;
        input.onKey(KeyCode::A, KeyState::Press);
        input.onKey(KeyCode::A, KeyState::Release);
        input.onMouseButton(MouseButton::Left, KeyState::Press);
        input.onMouseButton(MouseButton::Left, KeyState::Release);
        EXPECT_TRUE(input.key(KeyCode::A).pressed);
        EXPECT_TRUE(input.key(KeyCode::A).released);
        EXPECT_FALSE(input.key(KeyCode::A).down);
        EXPECT_TRUE(input.mouseButton(MouseButton::Left).pressed);
        EXPECT_TRUE(input.mouseButton(MouseButton::Left).released);
        EXPECT_FALSE(input.mouseButton(MouseButton::Left).down);
        input.onKey(KeyCode::A, KeyState::Press);
        EXPECT_TRUE(input.key(KeyCode::A).down);
        EXPECT_TRUE(input.key(KeyCode::A).released);
        input.onKey(KeyCode::B, KeyState::Repeat);
        EXPECT_FALSE(input.key(KeyCode::B).down);
        EXPECT_FALSE(input.key(KeyCode::B).pressed);
        input.onKey(KeyCode::Unknown, KeyState::Release);
        input.onKey(static_cast<KeyCode>(65535), KeyState::Press);
        input.onMouseButton(MouseButton::Unknown, KeyState::Press);
        input.onMouseButton(static_cast<MouseButton>(255), KeyState::Press);
        EXPECT_TRUE(input.key(KeyCode::A).down);
        EXPECT_FALSE(input.key(KeyCode::Unknown).released);
        EXPECT_FALSE(input.mouseButton(MouseButton::Unknown).pressed);
        EXPECT_FALSE(input.key(static_cast<KeyCode>(65535)).down);
    }

    TEST(InputStateTest, PointerAndWheelAccumulateAndCopiesOwnTheirValues)
    {
        InputState input;
        input.onMousePosition(100, 200);
        EXPECT_EQ(input.mouseDelta(), (PointerValue{}));
        input.onMousePosition(110, 190);
        input.onMousePosition(107, 205);
        input.onScroll(1, 2);
        input.onScroll(-0.5, -3);
        EXPECT_EQ(input.mousePosition(), (PointerValue{107, 205}));
        EXPECT_EQ(input.mouseDelta(), (PointerValue{7, 5}));
        EXPECT_EQ(input.scroll(), (PointerValue{0.5, -1}));
        input.onKey(KeyCode::World2, KeyState::Press);
        input.onMouseButton(MouseButton::X2, KeyState::Press);
        const auto snapshot = input;
        input.beginFrame();
        EXPECT_EQ(input.mouseDelta(), (PointerValue{}));
        EXPECT_EQ(input.scroll(), (PointerValue{}));
        EXPECT_EQ(input.mousePosition(), snapshot.mousePosition());
        input.onMousePosition(108, 203);
        input.onKey(KeyCode::World2, KeyState::Release);
        EXPECT_EQ(input.mouseDelta(), (PointerValue{1, -2}));
        EXPECT_EQ(snapshot.mouseDelta(), (PointerValue{7, 5}));
        EXPECT_TRUE(snapshot.key(KeyCode::World2).pressed);
        EXPECT_TRUE(snapshot.mouseButton(MouseButton::X2).down);
    }

    TEST(InputStateTest, FocusLossReleasesHoldsAndRecoveryRequiresNewInput)
    {
        InputState input;
        input.onKey(KeyCode::F, KeyState::Press);
        input.onMouseButton(MouseButton::Right, KeyState::Press);
        input.onMousePosition(10, 20);
        input.onMousePosition(30, 40);
        input.onScroll(0, 3);
        input.onFocus(false);
        EXPECT_FALSE(input.focused());
        EXPECT_TRUE(input.focusLost());
        EXPECT_TRUE(input.key(KeyCode::F).released);
        EXPECT_FALSE(input.key(KeyCode::F).down);
        EXPECT_FALSE(input.key(KeyCode::F).pressed);
        EXPECT_TRUE(input.mouseButton(MouseButton::Right).released);
        EXPECT_FALSE(input.mouseButton(MouseButton::Right).down);
        EXPECT_FALSE(input.mouseButton(MouseButton::Right).pressed);
        input.onFocus(false);
        input.onKey(KeyCode::F, KeyState::Press);
        input.onMouseButton(MouseButton::Right, KeyState::Press);
        input.onMousePosition(999, 999);
        input.onScroll(9, 9);
        EXPECT_EQ(input.mouseDelta(), (PointerValue{}));
        EXPECT_EQ(input.scroll(), (PointerValue{}));
        EXPECT_FALSE(input.key(KeyCode::F).down);
        input.onFocus(true);
        EXPECT_TRUE(input.focused());
        EXPECT_TRUE(input.focusLost());
        EXPECT_FALSE(input.mouseButton(MouseButton::Right).down);
        input.onMousePosition(500, 600);
        EXPECT_EQ(input.mouseDelta(), (PointerValue{}));
        input.beginFrame();
        EXPECT_FALSE(input.focusLost());
        EXPECT_FALSE(input.key(KeyCode::F).released);
        input.onMousePosition(503, 598);
        EXPECT_EQ(input.mouseDelta(), (PointerValue{3, -2}));
        input.onMouseButton(MouseButton::Right, KeyState::Press);
        EXPECT_TRUE(input.mouseButton(MouseButton::Right).pressed);
    }
}  // namespace
