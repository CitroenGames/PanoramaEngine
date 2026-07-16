// app.js -- Interactive behaviour for the 04_window_raster example.
//
// Button: counts clicks and updates the status label.
// TextEntry: echoes the current value to a status label on Enter.
// Slider: polls the value periodically and updates a status label.

var g_click_count = 0;

function OnButtonClick()
{
    g_click_count += 1;
    $('#ButtonStatus').text = 'Clicks: ' + g_click_count;
    $.Msg('Button clicked ' + g_click_count + ' time(s)');
}

// Poll the slider value every 0.1 seconds so the label stays in sync
// even during drag (the engine updates the value attribute directly).
function PollSlider()
{
    var slider = $('#VolumeSlider');
    var status = $('#SliderStatus');
    if (slider && status)
    {
        status.text = 'Value: ' + slider.GetAttributeString('value', '0');
    }
    $.Schedule(0.1, PollSlider);
}

// Submit the text entry value to the status label on Enter.
$('#NameEntry').SetPanelEvent('oninputsubmit', function ()
{
    var entry = $('#NameEntry');
    var status = $('#TextStatus');
    var val = entry.text;
    if (val && val.length > 0)
    {
        status.text = 'Hello, ' + val + '!';
    }
    else
    {
        status.text = '';
    }
});

$.Msg('app.js loaded');
PollSlider();
