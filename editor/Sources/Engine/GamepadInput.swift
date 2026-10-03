import CSkywalker
import GameController

/// Forwards connected game controllers (Xbox, PlayStation, Switch Pro, MFi) to the engine while the game plays.
///
/// The engine maps controller state to input actions (`input.json`), so nothing here knows about "jump" or
/// "move": it only reports sticks, triggers and buttons by position (south = A / Cross, ...).
@MainActor
final class GamepadBridge {
    private var announced = Set<Int>()

    init() {
        // Keep reading the controller only while the editor is in front: a game should not react to input meant for
        // another app.
        GCController.shouldMonitorBackgroundEvents = false
    }

    /// Sends the state of up to four controllers. Call once per frame while playing.
    func poll(into engine: OpaquePointer?) {
        guard let engine else { return }
        let controllers = GCController.controllers()
        for index in 0..<4 {
            if index < controllers.count, let pad = controllers[index].extendedGamepad {
                var state = SkyGamepad(
                    left_x: pad.leftThumbstick.xAxis.value, left_y: pad.leftThumbstick.yAxis.value,
                    right_x: pad.rightThumbstick.xAxis.value, right_y: pad.rightThumbstick.yAxis.value,
                    left_trigger: pad.leftTrigger.value, right_trigger: pad.rightTrigger.value,
                    buttons: Self.buttonMask(pad))
                let name = controllers[index].vendorName ?? "Controller"
                sky_input_gamepad(engine, Int32(index), 1, name, &state)
                announced.insert(index)
            } else if announced.remove(index) != nil {
                sky_input_gamepad(engine, Int32(index), 0, nil, nil)  // unplugged
            }
        }
    }

    /// Bit order matches `SkyGamepad.buttons` in sky_api.h.
    private static func buttonMask(_ pad: GCExtendedGamepad) -> UInt32 {
        let buttons: [(Bool, UInt32)] = [
            (pad.buttonA.isPressed, 0), (pad.buttonB.isPressed, 1), (pad.buttonX.isPressed, 2), (pad.buttonY.isPressed, 3),
            (pad.leftShoulder.isPressed, 4), (pad.rightShoulder.isPressed, 5),
            (pad.leftTrigger.isPressed, 6), (pad.rightTrigger.isPressed, 7),
            (pad.buttonOptions?.isPressed ?? false, 8), (pad.buttonMenu.isPressed, 9),
            (pad.leftThumbstickButton?.isPressed ?? false, 10), (pad.rightThumbstickButton?.isPressed ?? false, 11),
            (pad.dpad.up.isPressed, 12), (pad.dpad.down.isPressed, 13), (pad.dpad.left.isPressed, 14), (pad.dpad.right.isPressed, 15),
            (pad.buttonHome?.isPressed ?? false, 16),
        ]
        var mask: UInt32 = 0
        for (pressed, bit) in buttons where pressed { mask |= 1 << bit }
        return mask
    }
}
