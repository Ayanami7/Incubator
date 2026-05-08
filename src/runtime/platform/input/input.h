#pragma once

#include <GLFW/glfw3.h>

#include <cstdint>

namespace Incubator
{
    namespace Input
    {
        /// @brief 键盘按键码
        enum class KeyCode : uint16_t
        {
            Unknown = 0,

            // Letters A-Z
            A = 1,
            B = 2,
            C = 3,
            D = 4,
            E = 5,
            F = 6,
            G = 7,
            H = 8,
            I = 9,
            J = 10,
            K = 11,
            L = 12,
            M = 13,
            N = 14,
            O = 15,
            P = 16,
            Q = 17,
            R = 18,
            S = 19,
            T = 20,
            U = 21,
            V = 22,
            W = 23,
            X = 24,
            Y = 25,
            Z = 26,

            // Numbers 0-9
            Num0 = 27,
            Num1 = 28,
            Num2 = 29,
            Num3 = 30,
            Num4 = 31,
            Num5 = 32,
            Num6 = 33,
            Num7 = 34,
            Num8 = 35,
            Num9 = 36,

            // Function keys F1-F25
            F1 = 37,
            F2 = 38,
            F3 = 39,
            F4 = 40,
            F5 = 41,
            F6 = 42,
            F7 = 43,
            F8 = 44,
            F9 = 45,
            F10 = 46,
            F11 = 47,
            F12 = 48,
            F13 = 49,
            F14 = 50,
            F15 = 51,
            F16 = 52,
            F17 = 53,
            F18 = 54,
            F19 = 55,
            F20 = 56,
            F21 = 57,
            F22 = 58,
            F23 = 59,
            F24 = 60,
            F25 = 61,

            // Arrow and navigation keys
            Up = 62,
            Down = 63,
            Left = 64,
            Right = 65,
            PageUp = 66,
            PageDown = 67,
            Home = 68,
            End = 69,
            Insert = 70,
            Delete = 71,

            // Action keys
            Space = 72,
            Enter = 73,
            Escape = 74,
            Tab = 75,
            Backspace = 76,
            CapsLock = 77,

            // Symbol keys
            Apostrophe = 78,   // '
            Comma = 79,        // ,
            Minus = 80,        // -
            Period = 81,       // .
            Slash = 82,        // /
            Semicolon = 83,    // ;
            Equal = 84,        // =
            LeftBracket = 85,  // [
            RightBracket = 86, // ]
            Backslash = 87,    // backslash
            GraveAccent = 88,  // grave accent

            // Modifier keys
            LeftShift = 89,
            RightShift = 90,
            LeftControl = 91,
            RightControl = 92,
            LeftAlt = 93,
            RightAlt = 94,
            LeftSuper = 95,
            RightSuper = 96,

            // Numpad keys
            Kp0 = 97,
            Kp1 = 98,
            Kp2 = 99,
            Kp3 = 100,
            Kp4 = 101,
            Kp5 = 102,
            Kp6 = 103,
            Kp7 = 104,
            Kp8 = 105,
            Kp9 = 106,
            KpDecimal = 107,
            KpDivide = 108,
            KpMultiply = 109,
            KpSubtract = 110,
            KpAdd = 111,
            KpEnter = 112,
            KpEqual = 113,

            // Lock and system keys
            ScrollLock = 114,
            NumLock = 115,
            PrintScreen = 116,
            Pause = 117,
            Menu = 118,
            World1 = 119,
            World2 = 120
        };

        /// @brief 鼠标按键
        enum class MouseButton : uint8_t
        {
            Unknown = 0,
            Left = 1,
            Right = 2,
            Middle = 3,
            X1 = 4,  // side button 1 (back)
            X2 = 5   // side button 2 (forward)
        };

        /// @brief 按键状态
        enum class KeyState : uint8_t
        {
            Release = 0,
            Press = 1,
            Repeat = 2
        };

        /// @brief 将 GLFW 按键码转换为引擎统一的 KeyCode
        /// @param glfwKey GLFW 按键码（GLFW_KEY_* 常量）
        /// @return 对应的 KeyCode，未知按键返回 KeyCode::Unknown
        inline KeyCode fromGlfwKey(int glfwKey)
        {
            switch (glfwKey)
            {
                case GLFW_KEY_UNKNOWN:      return KeyCode::Unknown;

                // Letters
                case GLFW_KEY_A:            return KeyCode::A;
                case GLFW_KEY_B:            return KeyCode::B;
                case GLFW_KEY_C:            return KeyCode::C;
                case GLFW_KEY_D:            return KeyCode::D;
                case GLFW_KEY_E:            return KeyCode::E;
                case GLFW_KEY_F:            return KeyCode::F;
                case GLFW_KEY_G:            return KeyCode::G;
                case GLFW_KEY_H:            return KeyCode::H;
                case GLFW_KEY_I:            return KeyCode::I;
                case GLFW_KEY_J:            return KeyCode::J;
                case GLFW_KEY_K:            return KeyCode::K;
                case GLFW_KEY_L:            return KeyCode::L;
                case GLFW_KEY_M:            return KeyCode::M;
                case GLFW_KEY_N:            return KeyCode::N;
                case GLFW_KEY_O:            return KeyCode::O;
                case GLFW_KEY_P:            return KeyCode::P;
                case GLFW_KEY_Q:            return KeyCode::Q;
                case GLFW_KEY_R:            return KeyCode::R;
                case GLFW_KEY_S:            return KeyCode::S;
                case GLFW_KEY_T:            return KeyCode::T;
                case GLFW_KEY_U:            return KeyCode::U;
                case GLFW_KEY_V:            return KeyCode::V;
                case GLFW_KEY_W:            return KeyCode::W;
                case GLFW_KEY_X:            return KeyCode::X;
                case GLFW_KEY_Y:            return KeyCode::Y;
                case GLFW_KEY_Z:            return KeyCode::Z;

                // Numbers
                case GLFW_KEY_0:            return KeyCode::Num0;
                case GLFW_KEY_1:            return KeyCode::Num1;
                case GLFW_KEY_2:            return KeyCode::Num2;
                case GLFW_KEY_3:            return KeyCode::Num3;
                case GLFW_KEY_4:            return KeyCode::Num4;
                case GLFW_KEY_5:            return KeyCode::Num5;
                case GLFW_KEY_6:            return KeyCode::Num6;
                case GLFW_KEY_7:            return KeyCode::Num7;
                case GLFW_KEY_8:            return KeyCode::Num8;
                case GLFW_KEY_9:            return KeyCode::Num9;

                // Function keys
                case GLFW_KEY_F1:           return KeyCode::F1;
                case GLFW_KEY_F2:           return KeyCode::F2;
                case GLFW_KEY_F3:           return KeyCode::F3;
                case GLFW_KEY_F4:           return KeyCode::F4;
                case GLFW_KEY_F5:           return KeyCode::F5;
                case GLFW_KEY_F6:           return KeyCode::F6;
                case GLFW_KEY_F7:           return KeyCode::F7;
                case GLFW_KEY_F8:           return KeyCode::F8;
                case GLFW_KEY_F9:           return KeyCode::F9;
                case GLFW_KEY_F10:          return KeyCode::F10;
                case GLFW_KEY_F11:          return KeyCode::F11;
                case GLFW_KEY_F12:          return KeyCode::F12;
                case GLFW_KEY_F13:          return KeyCode::F13;
                case GLFW_KEY_F14:          return KeyCode::F14;
                case GLFW_KEY_F15:          return KeyCode::F15;
                case GLFW_KEY_F16:          return KeyCode::F16;
                case GLFW_KEY_F17:          return KeyCode::F17;
                case GLFW_KEY_F18:          return KeyCode::F18;
                case GLFW_KEY_F19:          return KeyCode::F19;
                case GLFW_KEY_F20:          return KeyCode::F20;
                case GLFW_KEY_F21:          return KeyCode::F21;
                case GLFW_KEY_F22:          return KeyCode::F22;
                case GLFW_KEY_F23:          return KeyCode::F23;
                case GLFW_KEY_F24:          return KeyCode::F24;
                case GLFW_KEY_F25:          return KeyCode::F25;

                // Arrow and navigation keys
                case GLFW_KEY_UP:           return KeyCode::Up;
                case GLFW_KEY_DOWN:         return KeyCode::Down;
                case GLFW_KEY_LEFT:         return KeyCode::Left;
                case GLFW_KEY_RIGHT:        return KeyCode::Right;
                case GLFW_KEY_PAGE_UP:      return KeyCode::PageUp;
                case GLFW_KEY_PAGE_DOWN:    return KeyCode::PageDown;
                case GLFW_KEY_HOME:         return KeyCode::Home;
                case GLFW_KEY_END:          return KeyCode::End;
                case GLFW_KEY_INSERT:       return KeyCode::Insert;
                case GLFW_KEY_DELETE:       return KeyCode::Delete;

                // Action keys
                case GLFW_KEY_SPACE:        return KeyCode::Space;
                case GLFW_KEY_ENTER:        return KeyCode::Enter;
                case GLFW_KEY_ESCAPE:       return KeyCode::Escape;
                case GLFW_KEY_TAB:          return KeyCode::Tab;
                case GLFW_KEY_BACKSPACE:    return KeyCode::Backspace;
                case GLFW_KEY_CAPS_LOCK:    return KeyCode::CapsLock;

                // Symbol keys
                case GLFW_KEY_APOSTROPHE:   return KeyCode::Apostrophe;
                case GLFW_KEY_COMMA:        return KeyCode::Comma;
                case GLFW_KEY_MINUS:        return KeyCode::Minus;
                case GLFW_KEY_PERIOD:       return KeyCode::Period;
                case GLFW_KEY_SLASH:        return KeyCode::Slash;
                case GLFW_KEY_SEMICOLON:    return KeyCode::Semicolon;
                case GLFW_KEY_EQUAL:        return KeyCode::Equal;
                case GLFW_KEY_LEFT_BRACKET: return KeyCode::LeftBracket;
                case GLFW_KEY_RIGHT_BRACKET:return KeyCode::RightBracket;
                case GLFW_KEY_BACKSLASH:    return KeyCode::Backslash;
                case GLFW_KEY_GRAVE_ACCENT: return KeyCode::GraveAccent;

                // Modifier keys
                case GLFW_KEY_LEFT_SHIFT:   return KeyCode::LeftShift;
                case GLFW_KEY_RIGHT_SHIFT:  return KeyCode::RightShift;
                case GLFW_KEY_LEFT_CONTROL: return KeyCode::LeftControl;
                case GLFW_KEY_RIGHT_CONTROL:return KeyCode::RightControl;
                case GLFW_KEY_LEFT_ALT:     return KeyCode::LeftAlt;
                case GLFW_KEY_RIGHT_ALT:    return KeyCode::RightAlt;
                case GLFW_KEY_LEFT_SUPER:   return KeyCode::LeftSuper;
                case GLFW_KEY_RIGHT_SUPER:  return KeyCode::RightSuper;

                // Numpad keys
                case GLFW_KEY_KP_0:         return KeyCode::Kp0;
                case GLFW_KEY_KP_1:         return KeyCode::Kp1;
                case GLFW_KEY_KP_2:         return KeyCode::Kp2;
                case GLFW_KEY_KP_3:         return KeyCode::Kp3;
                case GLFW_KEY_KP_4:         return KeyCode::Kp4;
                case GLFW_KEY_KP_5:         return KeyCode::Kp5;
                case GLFW_KEY_KP_6:         return KeyCode::Kp6;
                case GLFW_KEY_KP_7:         return KeyCode::Kp7;
                case GLFW_KEY_KP_8:         return KeyCode::Kp8;
                case GLFW_KEY_KP_9:         return KeyCode::Kp9;
                case GLFW_KEY_KP_DECIMAL:   return KeyCode::KpDecimal;
                case GLFW_KEY_KP_DIVIDE:    return KeyCode::KpDivide;
                case GLFW_KEY_KP_MULTIPLY:  return KeyCode::KpMultiply;
                case GLFW_KEY_KP_SUBTRACT:  return KeyCode::KpSubtract;
                case GLFW_KEY_KP_ADD:       return KeyCode::KpAdd;
                case GLFW_KEY_KP_ENTER:     return KeyCode::KpEnter;
                case GLFW_KEY_KP_EQUAL:     return KeyCode::KpEqual;

                // Lock and system keys
                case GLFW_KEY_SCROLL_LOCK:  return KeyCode::ScrollLock;
                case GLFW_KEY_NUM_LOCK:     return KeyCode::NumLock;
                case GLFW_KEY_PRINT_SCREEN: return KeyCode::PrintScreen;
                case GLFW_KEY_PAUSE:        return KeyCode::Pause;
                case GLFW_KEY_MENU:         return KeyCode::Menu;
                case GLFW_KEY_WORLD_1:      return KeyCode::World1;
                case GLFW_KEY_WORLD_2:      return KeyCode::World2;

                default:                    return KeyCode::Unknown;
            }
        }

        /// @brief 将 GLFW 鼠标按键码转换为引擎统一的 MouseButton
        /// @param glfwButton GLFW 鼠标按键码（GLFW_MOUSE_BUTTON_* 常量）
        /// @return 对应的 MouseButton，未知按键返回 MouseButton::Unknown
        inline MouseButton fromGlfwMouseButton(int glfwButton)
        {
            switch (glfwButton)
            {
                case GLFW_MOUSE_BUTTON_1: return MouseButton::Left;
                case GLFW_MOUSE_BUTTON_2: return MouseButton::Right;
                case GLFW_MOUSE_BUTTON_3: return MouseButton::Middle;
                case GLFW_MOUSE_BUTTON_4: return MouseButton::X1;
                case GLFW_MOUSE_BUTTON_5: return MouseButton::X2;
                default:                  return MouseButton::Unknown;
            }
        }

        /// @brief 将 GLFW 动作常量转换为引擎统一的 KeyState
        /// @param glfwAction GLFW 动作值（GLFW_RELEASE / GLFW_PRESS / GLFW_REPEAT）
        /// @return 对应的 KeyState
        inline KeyState fromGlfwAction(int glfwAction)
        {
            switch (glfwAction)
            {
                case GLFW_RELEASE: return KeyState::Release;
                case GLFW_PRESS:   return KeyState::Press;
                case GLFW_REPEAT:  return KeyState::Repeat;
                default:           return KeyState::Release;
            }
        }
    }  // namespace Input
}  // namespace Incubator
