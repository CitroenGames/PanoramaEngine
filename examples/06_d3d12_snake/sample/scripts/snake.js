// Panorama is the GUI/controller bridge only. The host receives these actions
// and updates the native game model; the JS never creates game-board panels.
function SnakeAction(action)
{
    $.__host('snake', action);
}

$.Msg('snake HUD loaded - board rendering stays in native D3D12');
