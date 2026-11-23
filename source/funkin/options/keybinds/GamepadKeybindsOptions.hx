package funkin.options.keybinds;

import flixel.effects.FlxFlicker;
import flixel.input.gamepad.FlxGamepad;
import flixel.input.gamepad.FlxGamepadInputID;
import flixel.text.FlxText;
import flixel.util.FlxColor;

/**
 * Controller binding menu. Every control has two slots; any controller input
 * (face buttons, D-pad, L/R, ZL/ZR, stick clicks and stick directions) can be bound.
 *
 * Up/Down: choose a control. Left/Right: choose a slot. Accept: rebind the slot,
 * then press the new input (wait 5 seconds to cancel). Y: clear the slot.
 * Back: save and leave.
 */
class GamepadKeybindsOptions extends MusicBeatSubstate {
	static final CAPTURE_TIMEOUT:Float = 5;

	static final CATEGORIES:Array<{name:String, controls:Array<String>}> = [
		{name: "category.notes", controls: ["NOTE_LEFT", "NOTE_DOWN", "NOTE_UP", "NOTE_RIGHT"]},
		{name: "category.ui", controls: ["LEFT", "DOWN", "UP", "RIGHT", "ACCEPT", "BACK", "PAUSE", "RESET", "CHANGE_MODE"]},
		{name: "category.engine", controls: ["SWITCHMOD", "FPS_COUNTER"]}
	];

	static final CONTROL_NAMES:Map<String, String> = [
		"NOTE_LEFT" => "left", "NOTE_DOWN" => "down", "NOTE_UP" => "up", "NOTE_RIGHT" => "right",
		"LEFT" => "left", "DOWN" => "down", "UP" => "up", "RIGHT" => "right",
		"ACCEPT" => "ui.accept", "BACK" => "ui.back", "PAUSE" => "ui.pause", "RESET" => "ui.reset",
		"CHANGE_MODE" => "ui.changeMode", "SWITCHMOD" => "engine.switchMod", "FPS_COUNTER" => "engine.fpsCounter"
	];

	static final NOTE_ICONS:Map<String, String> = [
		"NOTE_LEFT" => "purple0", "NOTE_DOWN" => "blue0", "NOTE_UP" => "green0", "NOTE_RIGHT" => "red0"
	];

	var settingCam:FlxCamera;
	var camFollow:FlxObject = new FlxObject(0, 0, 2, 2);
	var bg:FlxSprite;
	var hint:FlxText;

	var rows:Array<GamepadBindRow> = [];
	var curSelected:Int = 0;
	var curSlot:Int = 0;

	var capturing:GamepadBindRow = null;
	var captureTime:Float = 0;
	var captureFlicker:FlxFlicker = null;
	var waitForRelease:Bool = true;

	override function create() {
		super.create();

		settingCam = new FlxCamera();
		settingCam.bgColor = 0xCC000000;
		FlxG.cameras.add(settingCam, false);
		cameras = [settingCam];

		bg = new FlxSprite().loadAnimatedGraphic(Paths.image('menus/menuTransparent'));
		bg.scrollFactor.set();
		bg.alpha = 0.1;
		add(bg);

		var y:Float = 0;
		for (category in CATEGORIES) {
			y += 75;
			var title = new Alphabet(0, y, translateOr('KeybindsOptions.' + category.name, category.name), "bold");
			title.screenCenter(X);
			add(title);
			y += 75;

			for (control in category.controls) {
				var name = CONTROL_NAMES.get(control);
				var row = new GamepadBindRow(100, y, translateOr('KeybindsOptions.' + name, name), control, NOTE_ICONS.get(control));
				rows.push(row);
				add(row);
				y += 75;
			}
		}
		add(camFollow);

		hint = new FlxText(0, FlxG.height - 34, FlxG.width, "", 18);
		hint.setFormat(Paths.font("vcr.ttf"), 18, FlxColor.WHITE, CENTER, OUTLINE, FlxColor.BLACK);
		hint.scrollFactor.set();
		add(hint);

		settingCam.follow(camFollow, LOCKON, 0.125);
		changeSelection(0);
		updateHint();
	}

	override function destroy() {
		super.destroy();
		if (settingCam != null) FlxG.cameras.remove(settingCam);
	}

	static function translateOr(id:String, def:String):String
		return TU.exists(id) ? TU.translate(id) : def;

	function updateHint() {
		hint.text = capturing != null
			? 'Press any button, trigger or stick direction for "${capturing.title.text}"... (${Math.ceil(CAPTURE_TIMEOUT - captureTime)}s to cancel)'
			: 'Accept: Rebind    Left/Right: Slot    Y: Clear slot    Back: Save & exit';
	}

	override function update(elapsed:Float) {
		super.update(elapsed);

		if (capturing != null) {
			updateCapture(elapsed);
			return;
		}

		if (waitForRelease) {
			// don't let the press that opened this menu rebind anything
			if (!controls.ACCEPT_HOLD) waitForRelease = false;
			return;
		}

		if (controls.UP_P) changeSelection(-1);
		if (controls.DOWN_P) changeSelection(1);
		if (controls.LEFT_P || controls.RIGHT_P) {
			curSlot = (curSlot + 1) % GamepadBinds.SLOTS;
			CoolUtil.playMenuSFX(SCROLL, 0.7);
			changeSelection(0);
		}

		if (controls.ACCEPT) {
			startCapture();
		} else if (anyGamepadJustPressed(FlxGamepadInputID.X)) { // the button labelled Y
			var row = rows[curSelected];
			GamepadBinds.set(row.control, curSlot, FlxGamepadInputID.NONE);
			row.updateText();
			CoolUtil.playMenuSFX(CANCEL);
		} else if (controls.BACK) {
			Options.applyKeybinds();
			Options.save();
			CoolUtil.playMenuSFX(CANCEL);
			close();
		}
	}

	function startCapture() {
		capturing = rows[curSelected];
		captureTime = 0;
		waitForRelease = true;
		CoolUtil.playMenuSFX(CONFIRM);
		captureFlicker = FlxFlicker.flicker(capturing.binds[curSlot], 0, Options.flashingMenu ? 0.06 : 0.15, true, false);
		updateHint();
	}

	function stopCapture() {
		if (captureFlicker != null) {
			captureFlicker.stop();
			captureFlicker = null;
		}
		capturing.updateText();
		capturing = null;
		waitForRelease = true;
		updateHint();
	}

	function updateCapture(elapsed:Float) {
		captureTime += elapsed;
		if (captureTime >= CAPTURE_TIMEOUT) {
			CoolUtil.playMenuSFX(CANCEL);
			stopCapture();
			return;
		}
		updateHint();

		// wait until every controller is idle so the Accept press isn't captured
		if (waitForRelease) {
			if (!anyGamepadPressed()) waitForRelease = false;
			return;
		}

		for (gamepad in FlxG.gamepads.getActiveGamepads()) {
			var id:Int = gamepad.firstJustPressedID();
			if (id >= 0 && id != (FlxGamepadInputID.GUIDE : Int)) {
				GamepadBinds.set(capturing.control, curSlot, id);
				CoolUtil.playMenuSFX(CONFIRM);
				stopCapture();
				return;
			}
		}
	}

	function anyGamepadJustPressed(id:FlxGamepadInputID):Bool {
		for (gamepad in FlxG.gamepads.getActiveGamepads())
			if (gamepad.checkStatus(id, JUST_PRESSED)) return true;
		return false;
	}

	function anyGamepadPressed():Bool {
		for (gamepad in FlxG.gamepads.getActiveGamepads())
			if ((gamepad.firstPressedID() : Int) >= 0) return true;
		return false;
	}

	function changeSelection(change:Int) {
		if (change != 0) CoolUtil.playMenuSFX(SCROLL, 0.4);
		curSelected = FlxMath.wrap(curSelected + change, 0, rows.length - 1);

		for (i => row in rows) {
			row.alpha = i == curSelected ? 1 : 0.45;
			row.selectedSlot = curSlot;
		}

		var row = rows[curSelected];
		var last = rows[rows.length - 1];
		var minH = FlxG.height / 2;
		var maxH = last.y + last.height - (FlxG.height / 2) + 75;
		camFollow.setPosition(FlxG.width / 2, minH < maxH ? CoolUtil.bound(row.y + (row.height / 2), minH, maxH) : FlxG.height / 2);
	}
}

class GamepadBindRow extends FlxTypedSpriteGroup<FlxSprite> {
	public var control:String;
	public var title:Alphabet;
	public var binds:Array<Alphabet> = [];
	public var selectedSlot:Int = 0;

	public function new(x:Float, y:Float, name:String, control:String, ?noteAnim:String) {
		super();
		this.control = control;

		title = new Alphabet(0, 0, name, true);
		add(title);

		if (noteAnim != null) {
			var icon = new FlxSprite();
			icon.frames = Paths.getFrames("game/notes/default");
			icon.antialiasing = true;
			icon.animation.addByPrefix('icon', noteAnim, 24, true);
			icon.animation.play('icon');
			icon.setGraphicSize(75, 75);
			icon.updateHitbox();
			var min = Math.min(icon.scale.x, icon.scale.y);
			icon.scale.set(min, min);
			add(icon);
			title.x = 100;
		}

		for (slot in 0...GamepadBinds.SLOTS) {
			var b = new Alphabet(FlxG.width * (0.25 * (slot + 2)) - x - 40, 0, "", "normal");
			binds.push(b);
			add(b);
		}
		updateText();

		if (title.x + title.width > binds[0].x - 20) {
			title.scale.x = (binds[0].x - 20 - title.x) / title.width;
			title.updateHitbox();
		}

		setPosition(x, y);
	}

	public function updateText() {
		var ids = GamepadBinds.get(control);
		for (slot => b in binds) {
			b.text = GamepadBinds.label(ids[slot]);
			// keep long names like "D-Pad Right" inside their column
			var maxWidth = FlxG.width * 0.25 - 10;
			b.scale.x = b.textWidth > maxWidth ? maxWidth / b.textWidth : 1;
		}
	}

	override function update(elapsed:Float) {
		super.update(elapsed);
		for (slot => b in binds)
			b.alpha = alpha * (slot == selectedSlot ? 1 : 0.25);
	}
}
