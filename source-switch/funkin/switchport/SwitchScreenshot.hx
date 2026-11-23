package funkin.switchport;

import flixel.FlxG;
import funkin.backend.utils.CoolUtil;
import lime.graphics.Image;
import lime.graphics.ImageBuffer;
import lime.graphics.RenderContext;
import lime.utils.UInt8Array;

/**
 * Switch screenshot helper: hold ZL + ZR and press Minus to save the current frame
 * as a PNG in sdmc:/switch/CodenameEngine/screenshots/ (the system Capture button
 * does not work for homebrew in this setup).
 */
class SwitchScreenshot {
	static inline final DIR = "sdmc:/switch/CodenameEngine/screenshots/";
	static var requested:Bool = false;

	public static function init() {
		FlxG.signals.postUpdate.add(checkCombo);
		// added after OpenFL's stage renderer, so the frame is fully drawn but not yet swapped
		lime.app.Application.current.window.onRender.add(onRender);
	}

	static function checkCombo() {
		if (FlxG.gamepads == null) return;
		for (pad in FlxG.gamepads.getActiveGamepads())
			if (pad.pressed.LEFT_TRIGGER && pad.pressed.RIGHT_TRIGGER && pad.justPressed.BACK)
				requested = true;
	}

	static function onRender(context:RenderContext) {
		if (!requested) return;
		requested = false;
		try {
			var gl = context.webgl;
			var w = lime.app.Application.current.window.width, h = lime.app.Application.current.window.height;
			var pixels = new UInt8Array(w * h * 4);
			gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, pixels);

			// GL rows are bottom-up; flip and force opaque alpha
			var flipped = new UInt8Array(w * h * 4);
			var row = w * 4;
			for (y in 0...h) {
				var src = (h - 1 - y) * row, dst = y * row;
				for (i in 0...row) flipped[dst + i] = (i & 3) == 3 ? 255 : pixels[src + i];
			}

			var image = new Image(new ImageBuffer(flipped, w, h, 32, RGBA32));
			var png = image.encode(PNG);
			if (!sys.FileSystem.exists(DIR)) sys.FileSystem.createDirectory(DIR);
			var name = DIR + "shot-" + DateTools.format(Date.now(), "%Y%m%d-%H%M%S") + ".png";
			sys.io.File.saveBytes(name, png);
			openfl.CneBootLog.trace("[screenshot] saved " + name);
			CoolUtil.playMenuSFX(CONFIRM);
		} catch (e:Dynamic) {
			openfl.CneBootLog.trace("[screenshot] failed: " + Std.string(e));
		}
	}
}
