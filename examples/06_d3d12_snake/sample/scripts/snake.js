// This script is only the GUI/controller bridge. The host receives these
// actions and updates the native model; JS never creates level or snake panels.
function SnakeAction(action)
{
    $.__host('snake', action);
}

$.Msg('snake HUD loaded - board rendering stays in native D3D12');
