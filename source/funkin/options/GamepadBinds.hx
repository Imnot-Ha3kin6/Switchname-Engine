package funkin.options;

import flixel.input.actions.FlxActionInput.FlxInputDeviceID;
import flixel.input.gamepad.FlxGamepadInputID;
import funkin.backend.system.Controls.Control;

/**
 * Rebindable controller bindings (used on Nintendo Switch).
 *
 * Each control has two slots stored in `Options.GAMEPAD_<CONTROL>`, saved with
 * the other options. Values are `FlxGamepadInputID`s, `-1` means unbound.
 *
 * SDL reports Switch face buttons by *position* (Xbox layout), so the button
 * labelled A is `FlxGamepadInputID.B`, B is `A`, X is `Y` and Y is `X`.
 * `label()` converts an ID back to the name printed on the controller.
 */
class GamepadBinds {
	/** Controls that can be rebound from the controller menu, in display order. */
	public static final CONTROLS:Array<String> = [
		"NOTE_LEFT", "NOTE_DOWN", "NOTE_UP", "NOTE_RIGHT",
		"LEFT", "DOWN", "UP", "RIGHT", "ACCEPT", "BACK", "PAUSE", "RESET", "CHANGE_MODE",
		"SWITCHMOD", "FPS_COUNTER"
	];

	public static final SLOTS:Int = 2;

	/** Default bindings: left Joy-Con D-pad, or right Joy-Con Y/B/X/A for notes. */
	public static function getDefaults():Map<String, Array<Int>> {
		return [
			"NOTE_LEFT" => [FlxGamepadInputID.DPAD_LEFT, FlxGamepadInputID.X],    // D-pad left / Y
			"NOTE_DOWN" => [FlxGamepadInputID.DPAD_DOWN, FlxGamepadInputID.A],    // D-pad down / B
			"NOTE_UP" => [FlxGamepadInputID.DPAD_UP, FlxGamepadInputID.Y],        // D-pad up / X
			"NOTE_RIGHT" => [FlxGamepadInputID.DPAD_RIGHT, FlxGamepadInputID.B],  // D-pad right / A
			"LEFT" => [FlxGamepadInputID.DPAD_LEFT, FlxGamepadInputID.LEFT_STICK_DIGITAL_LEFT],
			"DOWN" => [FlxGamepadInputID.DPAD_DOWN, FlxGamepadInputID.LEFT_STICK_DIGITAL_DOWN],
			"UP" => [FlxGamepadInputID.DPAD_UP, FlxGamepadInputID.LEFT_STICK_DIGITAL_UP],
			"RIGHT" => [FlxGamepadInputID.DPAD_RIGHT, FlxGamepadInputID.LEFT_STICK_DIGITAL_RIGHT],
			"ACCEPT" => [FlxGamepadInputID.B, FlxGamepadInputID.NONE],            // A
			"BACK" => [FlxGamepadInputID.A, FlxGamepadInputID.NONE],              // B
			"PAUSE" => [FlxGamepadInputID.START, FlxGamepadInputID.NONE],         // +
			"RESET" => [FlxGamepadInputID.NONE, FlxGamepadInputID.NONE],          // unbound: Reset is an instant game over in songs
			"CHANGE_MODE" => [FlxGamepadInputID.BACK, FlxGamepadInputID.NONE],    // -
			"SWITCHMOD" => [FlxGamepadInputID.LEFT_SHOULDER, FlxGamepadInputID.NONE], // L
			"FPS_COUNTER" => [FlxGamepadInputID.NONE, FlxGamepadInputID.NONE]
		];
	}

	public static function get(control:String):Array<Int> {
		var binds:Array<Int> = Reflect.field(Options, 'GAMEPAD_$control');
		if (binds == null || binds.length < SLOTS) {
			var def = getDefaults().get(control);
			binds = def != null ? def.copy() : [for (_ in 0...SLOTS) NONE];
			Reflect.setField(Options, 'GAMEPAD_$control', binds);
		}
		return binds;
	}

	public static function set(control:String, slot:Int, id:Int) {
		var binds = get(control).copy();
		binds[slot] = id;
		Reflect.setField(Options, 'GAMEPAD_$control', binds);
	}

	public static function resetDefaults() {
		for (control => binds in getDefaults())
			Reflect.setField(Options, 'GAMEPAD_$control', binds.copy());
	}

	/** Binds the saved controller buttons to the solo and player 1 controls, for any connected controller. */
	public static function apply() {
		if (PlayerSettings.solo == null) return;
		for (settings in [PlayerSettings.solo, PlayerSettings.player1]) {
			if (settings == null) continue;
			var c = settings.controls;
			c.removeGamepad(FlxInputDeviceID.ALL);
			// Reset is an instant game over during songs, so never let it share a button with a note
			var noteButtons = [for (n in ["NOTE_LEFT", "NOTE_DOWN", "NOTE_UP", "NOTE_RIGHT"]) for (id in get(n)) id];
			for (name in CONTROLS) {
				var ids:Array<FlxGamepadInputID> = [for (id in get(name)) if (id >= 0 && !(name == "RESET" && noteButtons.contains(id))) (id : FlxGamepadInputID)];
				if (ids.length > 0) c.bindButtons(Type.createEnum(Control, name), FlxInputDeviceID.ALL, ids);
			}
		}
		applyDeadZone();
	}

	public static function applyDeadZone() {
		if (FlxG.gamepads != null)
			FlxG.gamepads.globalDeadZone = FlxMath.bound(Options.gamepadDeadZone, 5, 90) / 100;
	}

	/** The name printed on a Switch controller for this input. */
	public static function label(id:Int):String {
		return switch ((id : FlxGamepadInputID)) {
			case NONE: "---";
			case A: "B";
			case B: "A";
			case X: "Y";
			case Y: "X";
			case LEFT_SHOULDER: "L";
			case RIGHT_SHOULDER: "R";
			case LEFT_TRIGGER | LEFT_TRIGGER_BUTTON: "ZL";
			case RIGHT_TRIGGER | RIGHT_TRIGGER_BUTTON: "ZR";
			case START: "Plus";
			case BACK: "Minus";
			case GUIDE: "Home";
			case LEFT_STICK_CLICK: "L Stick Press";
			case RIGHT_STICK_CLICK: "R Stick Press";
			case DPAD_UP: "D-Pad Up";
			case DPAD_DOWN: "D-Pad Down";
			case DPAD_LEFT: "D-Pad Left";
			case DPAD_RIGHT: "D-Pad Right";
			case LEFT_STICK_DIGITAL_UP: "L Stick Up";
			case LEFT_STICK_DIGITAL_DOWN: "L Stick Down";
			case LEFT_STICK_DIGITAL_LEFT: "L Stick Left";
			case LEFT_STICK_DIGITAL_RIGHT: "L Stick Right";
			case RIGHT_STICK_DIGITAL_UP: "R Stick Up";
			case RIGHT_STICK_DIGITAL_DOWN: "R Stick Down";
			case RIGHT_STICK_DIGITAL_LEFT: "R Stick Left";
			case RIGHT_STICK_DIGITAL_RIGHT: "R Stick Right";
			default: "Button " + id;
		}
	}
}
