package funkin.switchport;

import flixel.FlxG;
import lime.app.Application;
import lime.ui.KeyCode;
import openfl.text.TextField;
import openfl.text.TextFieldType;

/**
 * Switch system keyboard for screens that need typing.
 *
 * - Text input fields (OpenFL TextField of type INPUT): when one gets focus, the keyboard opens
 *   with its current text and the result is written into the field.
 * - Screens that read raw key presses (e.g. a mod's "enter a code" menu): hold ZL + ZR and press
 *   Plus to open the keyboard; the typed text is sent as real key presses, one per frame, then Enter.
 */
@:cppFileCode('extern "C" int cne_swkbd(const char *initial, const char *guide, char *out, int outLen);')
class SwitchKeyboard {
	static var pending:Array<Int> = [];
	static var keyHeld:Int = -1;
	static var lastField:TextField = null;

	public static function init() {
		FlxG.signals.preUpdate.add(update);
	}

	/** Shows the system keyboard and returns the text, or null if it was cancelled. */
	public static function show(initial:String = "", guide:String = ""):Null<String> {
		var result:String = null;
		var buf = haxe.io.Bytes.alloc(512);
		var ok:Int = untyped __cpp__("cne_swkbd({0}.utf8_str(), {1}.utf8_str(), (char*){2}->GetBase(), 512)", initial, guide, buf.getData());
		if (ok == 1) {
			var len = 0;
			while (len < buf.length && buf.get(len) != 0) len++;
			result = buf.getString(0, len);
		}
		return result;
	}

	static function update() {
		var window = Application.current != null ? Application.current.window : null;
		if (window == null) return;

		// feed queued key presses: press on one frame, release on the next, so justPressed sees each key
		if (keyHeld != -1) {
			window.onKeyUp.dispatch(keyHeld, 0);
			keyHeld = -1;
		} else if (pending.length > 0) {
			keyHeld = pending.shift();
			window.onKeyDown.dispatch(keyHeld, 0);
		}

		// a text input field got focus: type straight into it
		var focus = FlxG.stage.focus;
		if (Std.isOfType(focus, TextField) && cast(focus, TextField).type == TextFieldType.INPUT) {
			var field:TextField = cast focus;
			if (field != lastField) {
				lastField = field;
				var text = show(field.text, "");
				if (text != null) field.text = text;
				FlxG.stage.focus = null;
			}
		} else {
			lastField = null;
		}

		// manual: ZL + ZR + Plus types into screens that read raw keys
		if (FlxG.gamepads != null && pending.length == 0 && keyHeld == -1)
			for (pad in FlxG.gamepads.getActiveGamepads())
				if (pad.pressed.LEFT_TRIGGER && pad.pressed.RIGHT_TRIGGER && pad.justPressed.START) {
					var text = show("", "Type here");
					if (text != null) queueText(text, true);
					break;
				}
	}

	/** Sends text as key presses (letters, digits, space and common symbols), optionally followed by Enter. */
	public static function queueText(text:String, pressEnter:Bool) {
		for (i in 0...text.length) {
			var c = text.toLowerCase().charCodeAt(i);
			if (c >= 32 && c < 127) pending.push(c); // Lime key codes match lowercase ASCII for printable keys
		}
		if (pressEnter) pending.push(KeyCode.RETURN);
	}
}
