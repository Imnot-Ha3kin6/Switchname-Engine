package funkin.switchport;

import flixel.FlxG;
import flixel.graphics.FlxGraphic;
import funkin.game.PlayState;
import lime.graphics.Image;
import openfl.display.BitmapData;
import openfl.utils.Assets;
import funkin.backend.assets.Paths;
import sys.thread.Deque;
import sys.thread.Thread;

/**
 * Background character preloading (Switch).
 *
 * When a song starts, every character named by a character-swap event in the chart is listed in song order.
 * A worker thread decodes the next characters' sprite sheets (the slow part) while the song keeps playing;
 * the main thread then uploads one finished sheet per frame and puts it in Flixel's bitmap cache under the
 * same key the character will ask for, so creating that character later doesn't decode or upload anything.
 *
 * Only characters; a few ahead at a time and only while there is memory to spare. A preloaded graphic is
 * kept until the character that needs it picks it up, then it is released like any other graphic.
 */
class CharacterPrefetcher {
	static inline final AHEAD:Int = 3;            // characters kept preloaded ahead of time
	static inline final MEMORY_LIMIT_MB:Int = 2300; // stop preloading above this

	static var queue:Array<{name:String, key:String, path:String}> = [];
	static var requests:Deque<{key:String, path:String}>;
	static var results:Deque<{key:String, image:Image}>;
	static var worker:Thread;
	static var inFlight:Int = 0;
	static var held:Array<FlxGraphic> = [];      // preloaded, waiting for their character
	static var active:Bool = false;

	public static function init() {
		requests = new Deque();
		results = new Deque();
		worker = Thread.create(workerLoop);
		FlxG.signals.postStateSwitch.add(onStateSwitch);
		FlxG.signals.preStateSwitch.add(stop);
		FlxG.signals.postUpdate.add(update);
	}

	static function workerLoop() {
		while (true) {
			var req = requests.pop(true);
			var image:Image = null;
			try image = Image.fromFile(req.path) catch (e:Dynamic) {}
			results.add({key: req.key, image: image});
		}
	}

	static function onStateSwitch() {
		if (Std.isOfType(FlxG.state, PlayState) && PlayState.SONG != null) start();
	}

	static function start() {
		stop();
		var seen = new Map<String, Bool>();
		var events:Array<Dynamic> = PlayState.SONG.events != null ? cast PlayState.SONG.events : [];
		events = events.copy();
		events.sort((a, b) -> Std.int((a.time : Float) - (b.time : Float)));
		for (ev in events) {
			var evName:String = Std.string(ev.name).toLowerCase();
			if (evName.indexOf("character") < 0 || ev.params == null) continue;
			for (p in (ev.params : Array<Dynamic>)) {
				if (!Std.isOfType(p, String)) continue;
				var name:String = p;
				if (seen.exists(name)) continue;
				var xmlPath = Paths.xml('characters/$name');
				if (!Assets.exists(xmlPath)) continue;
				seen.set(name, true);
				var sprite = name;
				try {
					var x = Xml.parse(Assets.getText(xmlPath)).firstElement();
					if (x != null && x.exists("sprite")) sprite = x.get("sprite");
				} catch (e:Dynamic) {}
				var key = Paths.image('characters/$sprite', null, false);
				if (key == null || !Assets.exists(key)) continue;   // atlas folders and missing sheets load normally
				var path = Assets.getPath(key);
				if (path == null) continue;
				queue.push({name: name, key: key, path: path});
			}
		}
		active = queue.length > 0;
		if (active) funkin.backend.system.Logs.trace('[CharacterPrefetcher] ${queue.length} swap characters to preload in the background');
	}

	static function stop() {
		active = false;
		queue = [];
		for (g in held) if (g != null && !g.isDestroyed) g.persist = false;
		held = [];
	}

	static function usedMB():Int {
		var s = openfl.CneBootLog.mem();
		return Std.parseInt(s.split("/")[0]);
	}

	static function update() {
		if (results == null) return;

		// release preloaded graphics once their character is using them
		var i = held.length;
		while (i-- > 0) {
			var g = held[i];
			if (g == null || g.isDestroyed) { held.splice(i, 1); continue; }
			if (g.useCount > 0) { g.persist = false; held.splice(i, 1); }
		}

		// upload at most one finished sheet per frame
		var r = results.pop(false);
		if (r != null) {
			inFlight--;
			if (r.image != null && FlxG.bitmap.get(r.key) == null) {
				var bmp = BitmapData.fromImage(r.image);
				if (bmp != null) {
					bmp.toHardware();   // GPU copy; frees the decoded CPU image
					var g = FlxGraphic.fromBitmapData(bmp, false, r.key, true);
					g.persist = true;   // survive cache cleanup until the character picks it up
					held.push(g);
				}
			}
		}

		// ask the worker for the next character
		if (!active || inFlight > 0 || queue.length == 0) return;
		if (held.length >= AHEAD || usedMB() > MEMORY_LIMIT_MB) return;
		while (queue.length > 0) {
			var next = queue.shift();
			if (FlxG.bitmap.get(next.key) != null) continue;   // already loaded by the game
			inFlight++;
			requests.add({key: next.key, path: next.path});
			break;
		}
	}
}
