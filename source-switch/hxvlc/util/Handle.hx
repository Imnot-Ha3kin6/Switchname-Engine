package hxvlc.util;

/**
 * Switch version of hxvlc's `Handle` (the libVLC instance manager).
 *
 * Video on Switch uses FFmpeg through `hxvlc.flixel.FlxVideoSprite` and needs no global
 * instance, so this only provides the same API so that mod scripts importing or calling
 * `Handle` keep working.
 */
@:keep
class Handle {
	/** Always loaded: the FFmpeg player needs no setup. */
	public static var loading(default, null):Bool = false;

	public static var version(get, never):String;
	public static var compiler(get, never):String;
	public static var changeset(get, never):String;

	public static function init(?options:Array<String>):Bool return true;

	public static function initAsync(?options:Array<String>, ?finishCallback:Bool->Void):Void {
		if (finishCallback != null) finishCallback(true);
	}

	public static function dispose():Void {}

	static function get_version():String return "FFmpeg (Switch)";
	static function get_compiler():String return "devkitA64";
	static function get_changeset():String return "";
}
