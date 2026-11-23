package funkin.switchport;

import flixel.FlxG;
import flixel.input.gamepad.FlxGamepadInputID;
import lime.app.Application;
import lime.ui.MouseButton;

/**
 * Controller-driven mouse cursor for screens that need one (Switch has no mouse).
 *
 * While the game shows the mouse cursor (`FlxG.mouse.visible`), the right stick moves it and
 * ZR / ZL act as left / right click. The input is fed through Lime's window mouse events, so
 * Flixel (`FlxG.mouse`) and OpenFL buttons react exactly as they would to a real mouse.
 * The touchscreen already works as a mouse through OpenFL's primary-touch handling.
 */
class SwitchCursor {
	static inline final SPEED:Float = 1100; // window pixels per second at full tilt
	static inline final DEAD_ZONE:Float = 0.18;

	static var x:Float = 640;
	static var y:Float = 360;
	static var leftDown:Bool = false;
	static var rightDown:Bool = false;

	public static function init() {
		FlxG.signals.preUpdate.add(update);
	}

	static function update() {
		if (FlxG.gamepads == null) return;
		var window = Application.current != null ? Application.current.window : null;
		if (window == null) return;

		if (!FlxG.mouse.visible) {
			// never leave a click held when the cursor goes away
			if (leftDown) { leftDown = false; window.onMouseUp.dispatch(x, y, MouseButton.LEFT); }
			if (rightDown) { rightDown = false; window.onMouseUp.dispatch(x, y, MouseButton.RIGHT); }
			return;
		}

		var dx:Float = 0, dy:Float = 0, left = false, right = false;
		for (pad in FlxG.gamepads.getActiveGamepads()) {
			var sx = pad.getXAxis(RIGHT_ANALOG_STICK), sy = pad.getYAxis(RIGHT_ANALOG_STICK);
			if (Math.abs(sx) > Math.abs(dx)) dx = sx;
			if (Math.abs(sy) > Math.abs(dy)) dy = sy;
			left = left || pad.pressed.RIGHT_TRIGGER;
			right = right || pad.pressed.LEFT_TRIGGER;
		}

		var mag = Math.sqrt(dx * dx + dy * dy);
		if (mag > DEAD_ZONE) {
			// ease in so small tilts allow precise aiming
			var scale = SPEED * FlxG.elapsed * Math.pow((mag - DEAD_ZONE) / (1 - DEAD_ZONE), 1.6) / mag;
			x = Math.max(0, Math.min(window.width - 1, x + dx * scale));
			y = Math.max(0, Math.min(window.height - 1, y + dy * scale));
			window.onMouseMove.dispatch(x, y);
		}

		if (left != leftDown) {
			leftDown = left;
			left ? window.onMouseDown.dispatch(x, y, MouseButton.LEFT) : window.onMouseUp.dispatch(x, y, MouseButton.LEFT);
		}
		if (right != rightDown) {
			rightDown = right;
			right ? window.onMouseDown.dispatch(x, y, MouseButton.RIGHT) : window.onMouseUp.dispatch(x, y, MouseButton.RIGHT);
		}
	}
}
