package funkin.backend.scripting;

import flixel.FlxState;
import funkin.backend.assets.ModsFolder;
import funkin.backend.scripting.events.CancellableEvent;
import funkin.backend.system.Conductor;
import funkin.options.PlayerSettings;

#if GLOBAL_SCRIPT
/**
 * Class for THE Global Script, aka script that runs in the background at all times.
 */
class GlobalScript {
	public static var scripts:ScriptPack;

	private static var initialized:Bool = false;
	private static var reloading:Bool = false;
	private static var _lastAllow_Reload:Bool = false;

	/** Custom screens a mod used in place of a built-in menu (screen name -> menu class), learned while its custom menus are off. */
	static var replacedMenus:Map<String, Class<FlxState>> = [];

	/** True when the current mod's custom menus are turned off (Options.modsWithoutCustomMenus). */
	public static function customMenusDisabled():Bool
		return ModsFolder.currentModFolder != null && funkin.options.Options.modsWithoutCustomMenus.contains(ModsFolder.currentModFolder);

	static function isMenuState(state:Dynamic):Bool {
		if (state == null || !Std.isOfType(state, FlxState)) return false;
		var name = Type.getClassName(Type.getClass(state));
		return StringTools.startsWith(name, "funkin.menus.") || name == "funkin.options.OptionsMenu";
	}

	static function customScreenName(state:Dynamic):String
		return Std.isOfType(state, ModState) ? cast(state, ModState).scriptName : null;

	/**
	 * Undo a mod's menu takeover when its custom menus are off: keep the built-in menu that was
	 * requested, and remember which custom screen replaced it so direct switches to that screen
	 * later open the built-in menu instead.
	 */
	static function guardMenus(before:Dynamic, gameStart:Bool) {
		if (!customMenusDisabled()) return;
		var now:Dynamic = @:privateAccess FlxG.game._requestedState;
		var custom = customScreenName(now);

		if (now != before && custom != null && (gameStart || before == null || isMenuState(before))) {
			// the mod swapped a built-in menu (or the startup screen) for its own screen
			replacedMenus.set(custom, before != null ? cast Type.getClass(before) : funkin.menus.TitleState);
			@:privateAccess FlxG.game._requestedState = cast before;
			// the mod's custom transition goes with its custom screens; the default menus need the default one
			funkin.backend.MusicBeatTransition.script = Flags.DEFAULT_TRANSITION_SCRIPT;
			Logs.trace('Custom menus off for ${ModsFolder.currentModFolder}: kept the default menu instead of "$custom"', INFO);
		} else if (custom != null && replacedMenus.exists(custom)) {
			// the mod opened one of its menu screens directly (e.g. after a cutscene)
			@:privateAccess FlxG.game._requestedState = cast Type.createInstance(replacedMenus.get(custom), []);
		}
	}

	public static function init() {
		if(initialized) return;
		initialized = true;
		#if MOD_SUPPORT
		ModsFolder.onModSwitch.add(onModSwitch);
		#end

		Conductor.onBeatHit.add(beatHit);
		Conductor.onStepHit.add(stepHit);

		FlxG.signals.focusGained.add(function() {
			call("focusGained");
		});
		FlxG.signals.focusLost.add(function() {
			call("focusLost");
		});
		FlxG.signals.gameResized.add(function(w:Int, h:Int) {
			call("gameResized", [w, h]);
		});
		FlxG.signals.postDraw.add(function() {
			call("postDraw");
		});
		FlxG.signals.postGameReset.add(function() {
			call("postGameReset");
		});
		FlxG.signals.postGameStart.add(function() {
			var before:Dynamic = @:privateAccess FlxG.game._requestedState;
			call("postGameStart");
			guardMenus(before, true);
		});
		FlxG.signals.postStateSwitch.add(function() {
			call("postStateSwitch");
		});
		FlxG.signals.postUpdate.add(function() {
			call("postUpdate", [FlxG.elapsed]);

			if (reloading) {
				reloading = false;
				MusicBeatState.ALLOW_DEV_RELOAD = _lastAllow_Reload;
			}
		});
		FlxG.signals.preDraw.add(function() {
			call("preDraw");
		});
		FlxG.signals.preGameReset.add(function() {
			call("preGameReset");
		});
		FlxG.signals.preGameStart.add(function() {
			call("preGameStart");
		});
		FlxG.signals.preStateCreate.add(function(state:FlxState) {
			call("preStateCreate", [state]);
		});
		FlxG.signals.preStateSwitch.add(function() {
			var before:Dynamic = @:privateAccess FlxG.game._requestedState;
			call("preStateSwitch");
			guardMenus(before, false);

			if (Flags.PATHS_CACHE_RESET_ON_SWITCH_STATE) Paths.assetsTree.resetAssetPathCache();

			var stateName = Type.getClassName(Type.getClass(@:privateAccess FlxG.game._requestedState));
			stateName = stateName.substring(stateName.lastIndexOf(".") + 1);
			if (Flags.MOD_REDIRECT_STATES.exists(stateName) && !(customMenusDisabled() && isMenuState(@:privateAccess FlxG.game._requestedState))) {
				@:privateAccess {
					var classFromString = Type.resolveClass(Flags.MOD_REDIRECT_STATES.get(stateName));
					if (classFromString != null) FlxG.game._requestedState = Type.createInstance(classFromString, []);
					else FlxG.game._requestedState = new funkin.backend.scripting.ModState(Flags.MOD_REDIRECT_STATES.get(stateName));
				}
			}
		});

		FlxG.signals.preUpdate.add(function() {
			call("preUpdate", [FlxG.elapsed]);
			call("update", [FlxG.elapsed]);

			if (FlxG.keys.pressed.SHIFT) {
				// If we want, we could just make reseting GlobalScript it's own keybind, but for now this works.
				if (PlayerSettings.solo.controls.DEV_RELOAD) {
					reloading = true;
					Logs.trace("Reloading Global Scripts...", INFO, YELLOW);

					// yeah its a bit messy, sorry. This just prevents actually reloading the actual state.
					_lastAllow_Reload = MusicBeatState.ALLOW_DEV_RELOAD;
					MusicBeatState.ALLOW_DEV_RELOAD = false;

					// Would be better to just re-initalize GlobalScript so there aren't any lose ends.
					onModSwitch(#if MOD_SUPPORT ModsFolder.currentModFolder #else null #end);
				}
			}
		});
	}

	public static function onModSwitch(newMod:String) {
		destroy();
		scripts = new ScriptPack("GlobalScript");

		// global folder shit -HeroEyad
		var folders = ["data/globals"];
		while (folders.length > 0) {
			var folder = folders.shift();
			for (file in Paths.getFolderContent(folder, true)) {
				if (!Script.scriptExtensions.contains(haxe.io.Path.extension(file)) || haxe.io.Path.withoutDirectory(file).startsWith("LIB_")) continue;
				var script = Script.create(Paths.script(file));
				if (script is DummyScript) continue;
				scripts.add(script);
				script.load();
			}
			for (dir in Paths.getFolderDirectories(folder, true))
				folders.push(dir);
		}

		for (lib in funkin.backend.assets.ModsFolder.getLoadedModsLibs()) {
			var modName = lib.modName;
			var path = Paths.script('data/global/LIB_$modName');
			var script = Script.create(path);
			if (script is DummyScript) continue;
			script.remappedNames.set(script.fileName, '$modName:${script.fileName}');
			// so you can get the current mod's library in GloablScript :)
			// you should not make this a static variable then all scripts will try to reference the 1 static variable, which will be overwritten :yoikes:
			script.set("MOD_LIBRARY", lib);
			scripts.add(script);
			script.load();
		}
	}

	public static inline function event<T:CancellableEvent>(name:String, event:T):T {
		if (scripts != null)
			scripts.event(name, event);
		return event;
	}

	public static inline function call(name:String, ?args:Array<Dynamic>)
		if (scripts != null) scripts.call(name, args);

	public static inline function beatHit(curBeat:Int)
		call("beatHit", [curBeat]);

	public static inline function stepHit(curStep:Int)
		call("stepHit", [curStep]);

	public static inline function destroy() if (scripts != null) {
		call("destroy");
		scripts = FlxDestroyUtil.destroy(scripts);
	}
}
#end