package funkin.options.categories;

import funkin.options.keybinds.GamepadKeybindsOptions;

/**
 * Controller settings (Nintendo Switch): rebind buttons, stick dead zone and reset.
 */
class ControllerOptions extends TreeMenuScreen {
	public function new() {
		super('optionsTree.controller-name', 'optionsTree.controller-desc', 'ControllerOptions.');

		add(new TextOption(getNameID('bindings'), getDescID('bindings'), ' >', () -> {
			parent.persistentUpdate = false;
			parent.persistentDraw = true;
			parent.openSubState(new GamepadKeybindsOptions());
		}));
		add(new NumOption(getNameID('deadZone'), getDescID('deadZone'), 5, 90, 5, 'gamepadDeadZone', (_) -> GamepadBinds.applyDeadZone()));

		add(new Separator());
		add(new TextOption(getNameID('resetBindings'), getDescID('resetBindings'), () -> {
			GamepadBinds.resetDefaults();
			Options.gamepadDeadZone = 25;
			Options.applyKeybinds();
			Options.save();
			CoolUtil.playMenuSFX(CONFIRM);
			parent.updateDesc(translate('resetBindings-done'));
		}));
	}
}
