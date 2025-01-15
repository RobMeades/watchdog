/*
 * Copyright 2025 Rob Meades
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/** @file
 * @brief Javascript for index.html.  This code requires jquery.js
 * and hls.js.
 */

/* ----------------------------------------------------------------
 * HLS VIDEO PLAYING STUFF
 * -------------------------------------------------------------- */

'use strict';
const gPlayButton = document.getElementById('play');
const gVideo = document.getElementById('video');

function startPlaying() {
    // For mobile browsers the start of playing has to
    // be performed by a user action otherwise it will
    // be ignored
    gPlayButton.addEventListener('click', function() {
        gVideo.play();
        gVideo.muted = true;
        gPlayButton.hidden = true;
    });
    gPlayButton.hidden = false;
}

if (Hls.isSupported()) {
    const config = {
      debug: true,
      liveSyncDurationCount: 3,
      liveMaxLatencyDurationCount: 5,
      maxLiveSyncPlaybackRate: 2
    };

    const hls = new Hls(config);

    // This puts up alert boxes in the browser that need to be dismissed
    // before continuing
    //hls.on(Hls.Events.ERROR, function (event, data) {
    //  alert("HLS error: \n" + JSON.stringify(data, null, 4));
    //});

    hls.loadSource('video/watchdog.m3u8');
    hls.attachMedia(gVideo);
    hls.on(Hls.Events.MANIFEST_PARSED, startPlaying);
    hls.on(Hls.Events.ERROR, function(event, data) {
        if (data.fatal) {
            switch (data.type) {
                case Hls.ErrorTypes.NETWORK_ERROR:
                    // try to recover network error
                    console.log("fatal network error, trying to recover");
                    hls.startLoad();
                break;
                case Hls.ErrorTypes.MEDIA_ERROR:
                    console.log("fatal media error, trying to recover");
                    hls.recoverMediaError();
                break;
                default:
                    console.log("unhandled error (" + data.type + ")");
                break;
            }
        }
    });
} else if (gVideo.canPlayType('application/vnd.apple.mpegurl')) {
    // hls.js is not supported on platforms that do not have Media Source Extensions (MSE) enabled.
    // When the browser has built-in HLS support (check using `canPlayType`), we can provide an HLS manifest (i.e. .m3u8 URL) directly to the gVideo element through the `src` property.
    // This is using the built-in support of the plain gVideo element, without using hls.js.
    gVideo.src = 'video/watchdog.m3u8';
    gVideo.addEventListener('loadedmetadata', startPlaying);
}

/* ----------------------------------------------------------------
 * NOTIFICATION STUFF
 * -------------------------------------------------------------- */

// Grab the ID of the notification box.
const gNotification = document.getElementById('notification');

// Show the notification box for the given time.
function showNotification(message, timeoutMs = 5000) {
    gNotification.textContent = message;
    gNotification.style.display = 'block'
    gNotification.classList.add('show');

    // Hide the notification after the specified time
    setTimeout(() => {
        gNotification.classList.remove('show');
        gNotification.classList.add('hide');

        // Hide notification after the fade-out animation
        setTimeout(() => {
            gNotification.classList.remove('hide');
            // This should be the same as the duration of
            // CSS .notification-hide (in milliseconds)
        }, 500);
    }, timeoutMs);
}

/* ----------------------------------------------------------------
 * WEEKLY SCHEDULE STUFF: EVENT LISTENERS, DIALOGS AND THE LIKE
 * -------------------------------------------------------------- */

// Grab the IDs of the elements of the schedule change dialogue box.
const gDialog = document.getElementById('schedule-change');
const gDialogCloseBtn = document.getElementById('dialog-close-btn');
const gSubmitBtn = document.getElementById('schedule-change-submit-btn');
const gRadioMotors = document.getElementById('radio-motors');
const gRadioLights = document.getElementById('radio-lights');

// Event listener: close the schedule-change dialogue box
// when the close button is clicked.
gDialogCloseBtn.addEventListener('click', () => {
    gDialog.style.display = 'none';
    if (gLastFocusedElement) {
        gLastFocusedElement.focus();
    }
});

// Event listener: close the schedule-change dialog when
// clicking outside the dialogue.
window.addEventListener('click', (event) => {
    if (event.target === gDialog) {
        gDialog.style.display = 'none';
        if (gLastFocusedElement) {
            gLastFocusedElement.focus();
        }
    }
});

// These variables and the functions that follow should
// be inside the 'DOMContentLoaded' table manipulation
// event listener below; however, in order to support
// undo/redo, they have to be accessible to the
// 'keydown' event listener and hence need to be at
// the top level.
let gLastSelectedCell = null;
let gNumCellsSelected = 0;
let gSelectionHistory = [];
let gCurrentStateIndex = -1;
let gTable; // Will be populated by the 'DOMContentLoaded' event listener below
let gIsCtrlPressed = false;
let gLastFocusedElement;

// Toggle selection of the given cell.
function toggleCellSelection(cell) {
    let numCells = 0;
    if (cell.classList.contains('selected')) {
        cell.classList.remove('selected');
        numCells--;
    } else {
        cell.classList.add('selected');
        numCells++;
    }
    return numCells;
}

// Save the currently selected cells for undo/redo.
function saveSelectionState(zero = false) {
    let selectedState = null;
    if (!zero) {
        // Get the current cell selection state
        const selectedCells = gTable.querySelectorAll('td.selected');
        selectedState = Array.from(selectedCells).map(cell => ({
            row: cell.parentElement.rowIndex,
            col: cell.cellIndex
        }));
    }

    // Clear the redo history if a new selection is made after undo
    if (gCurrentStateIndex < gSelectionHistory.length - 1) {
        gSelectionHistory.splice(gCurrentStateIndex + 1);
    }

    // Add the new state to the history
    gSelectionHistory.push(selectedState);
    gCurrentStateIndex = gSelectionHistory.length - 1;
}

// Clear all selections.
function clearSelection() {
    let selectedCells = gTable.querySelectorAll('td.selected');
    selectedCells.forEach(cell => cell.classList.remove('selected'));
    return 0;
}

// Event listener: schedule-change form submission.
gSubmitBtn.addEventListener('click', () => {
    // Remove the dialog box
    gDialog.style.display = 'none';

    // Get the selected radio buttons
    const selectedMotors = document.querySelector('input[name="motors"]:checked');
    const selectedLights = document.querySelector('input[name="lights"]:checked');

    let notification = null;
    if (selectedMotors) {
        notification = 'motors ' + selectedMotors.value;
    }
    if (selectedLights) {
        if (selectedMotors) {
            notification += ', ';
        } else {
            notification = '';
        }
        notification += 'lights ' + selectedLights.value;
    }

    let isConfirmed = false;
    if (notification != null) {
        // Display an "are you sure" dialog box using the browser's native confirm() mechanism
        isConfirmed = confirm('Set ' + notification + ': are you sure?');
        if (isConfirmed) {
            notification = 'Changes successfully written';
        } else {
            notification = 'Cancelled';
        }
    } else {
        notification = 'Nothing to do';
    }

    if (notification != null) {
        showNotification(notification);
    }
    if (isConfirmed) {
        // Clear selection and selection state history,
        // adding a new zero state
        clearSelection();
        tableSelectionStateHistoryClear();
        saveSelectionState(true);
        gLastSelectedCell = null;
        gNumCellsSelected = 0;
    }
    if (gLastFocusedElement) {
        gLastFocusedElement.focus();
    }
});

// Engage the schedule-change modal dialog box.
function scheduleChange() {
    const radioButtons = document.querySelectorAll('input[type="radio"]');
    // Uncheck the radio buttons before displaying them,
    // as a kind of reset mechanism in case the user
    // ticks one they didn't intend to
    radioButtons.forEach(radio => {
        radio.checked = false;
    });

    // Show the dialog box; event listeners will close it
    gDialog.style.display = 'flex';
    // Since the dialog is a div, we need to move focus manually
    gLastFocusedElement = document.activeElement;
    gDialog.focus();
}

// Retore the state of cell selection, used by table undo/redo.
function tableRestoreSelectionState() {
    // Clear the current selection
    clearSelection();

    // Restore the selection state from history
    const selectedState = gSelectionHistory[gCurrentStateIndex];
    gLastSelectedCell = null;
    gNumCellsSelected = 0;
    if (selectedState) {
        selectedState.forEach(({ row, col }) => {
            const cell = gTable.rows[row].cells[col];
            cell.classList.add('selected');
            gNumCellsSelected++;
        });
    }
}

// Clear selection state history.
function tableSelectionStateHistoryClear() {
    gSelectionHistory = [];
    gCurrentStateIndex = -1;
}

// Undo table cell selection.
function tableUndo() {
    if (gCurrentStateIndex > 0) {
        gCurrentStateIndex--; // Move to the previous state
        tableRestoreSelectionState();
    }
}

// Redo table cell selection.
function tableRedo() {
    if (gCurrentStateIndex < gSelectionHistory.length - 1) {
        gCurrentStateIndex++; // Move to the next state
        tableRestoreSelectionState();
    }
}

// Event listener: undo/redo keys for cell selection,
// done in a slightly peculiar way as the browser's own
// undo functionality interferes with the key-presses
// we receive.
document.addEventListener('keydown', function(event) {
    if (event.key === 'Control' || event.key === 'Meta') {
        gIsCtrlPressed = true;
        // Ctrl-Z (undo)
    } else if (gIsCtrlPressed && (event.key === 'z' || event.key === 'Z')) {
        event.preventDefault();
        tableUndo();
        // Ctrl-Y (redo)
    } else if (gIsCtrlPressed && (event.key === 'y' || event.key === 'Y')) {
        event.preventDefault();
        tableRedo();
    }
});

// Event listener: key up for undo/redo key handling.
document.addEventListener('keyup', function(event) {
    if (event.key === 'Control' || event.key === 'Meta') {
        gIsCtrlPressed = false;
    }
});

// A function which is attached to the table via an event listener
// (see 'DOMContentLoaded') and (a) makes the tab ordering go down
// the columns rather than across the rows, (b) allows arrow keys
// to be used for navigation and (c) handles <enter> to select a
// cell and CTRL-<enter> to submit a change.
function tableHandleKeydown(event) {
    if (event.target.tagName === 'TD') {
        if (event.key === 'Tab' || event.key === 'ArrowLeft' ||
            event.key === 'ArrowRight' || event.key === 'ArrowUp' ||
            event.key === 'ArrowDown') {
            event.preventDefault();

            const table = event.currentTarget;
            const rows = Array.from(table.querySelectorAll('tr'));
            const currentColumn = event.target;
            const currentRow = currentColumn.parentElement;
            const columns = Array.from(currentRow.querySelectorAll('td'));
            const currentColumnIndex = Array.from(currentRow.children).indexOf(currentColumn);
            const currentRowIndex = rows.indexOf(currentRow);

            // The use of 1 all over the place below is
            // because we don't want to select the first
            // row or the first column
            let newColumnIndex = currentColumnIndex;
            let newRowIndex = currentRowIndex;
            if (event.key === 'Tab') {
                // Tab ordering
                if (event.shiftKey) {
                    // Move to the previous row (up)
                    newRowIndex--;
                    if (newRowIndex < 1) {
                        newColumnIndex--;
                        newRowIndex = rows.length - 1;
                    }
                } else {
                    // Move to the next row (down)
                    newRowIndex = currentRowIndex + 1;
                    if (newRowIndex >= rows.length) {
                        newColumnIndex++;
                        newRowIndex = 1;
                    }
                }
                // All of the arrow keys below loop in a circle
            } else if (event.key === 'ArrowRight') {
                // Right
                newColumnIndex++;
                if (newColumnIndex >= columns.length) {
                    newColumnIndex = 1;
                }
            } else if (event.key === 'ArrowLeft') {
                // Left
                newColumnIndex--;
                if (newColumnIndex < 1) {
                    newColumnIndex = columns.length - 1;
                }
            } else if (event.key === 'ArrowUp') {
                // Up
                newRowIndex--;
                if (newRowIndex < 1) {
                    newRowIndex = rows.length - 1;
                }
            } else if (event.key === 'ArrowDown') {
                // Down
                newRowIndex++;
                if (newRowIndex >= rows.length) {
                    newRowIndex = 1;
                }
            }
            if ((newColumnIndex >= 1) && (newColumnIndex < columns.length)) {
                const newRow = rows[newRowIndex];
                const newColumn = newRow.children[newColumnIndex];
                newColumn.focus();
            } else {
                currentColumn.blur();
            }
        } else if (event.key === 'Enter') {
            event.preventDefault();
            if (!gIsCtrlPressed) {
                gNumCellsSelected += toggleCellSelection(event.target);
                saveSelectionState();
            } else {
                if (gNumCellsSelected > 0) {
                    // Do the schedule change
                    scheduleChange();
                }
            }
        }
    }
}

// Event listener: handle cells of the table being selected.
document.addEventListener('DOMContentLoaded', function() {
    const table = document.getElementById('week');
    let clickTimerList = [];
    let delayMs = 300;
    let isDragging = false;
    let startCell = null;

    // Unfortunately has to be global (for undo/redo stuff)
    gTable = document.getElementById('week');

    // Event listeners: for mouse-based cell selection
    gTable.addEventListener('mousedown', handleMouseDown);
    gTable.addEventListener('mousemove', handleMouseMove);
    gTable.addEventListener('mouseup', handleMouseUp);

    // Start a click and drag.
    function handleMouseDown(event) {
        const cell = event.target;
        if (cell.tagName === 'TD') {
            isDragging = true;
            startCell = cell;
        }
    }

    // End a click and drag.
    function handleMouseMove(event) {
        if (isDragging) {
            const cell = event.target;
            if (cell.tagName === 'TD') {
                gNumCellsSelected += selectRange(startCell, cell);
            }
        }
    }

    // No more dragging; save state.
    function handleMouseUp() {
        isDragging = false;
        saveSelectionState();
    }

    // Select a range of cells.
    function selectRange(startCell, endCell) {
        let startRow = startCell.parentElement.rowIndex;
        let startCol = startCell.cellIndex;
        let endRow = endCell.parentElement.rowIndex;
        let endCol = endCell.cellIndex;
        let numCells = 0;

        // Determine the range boundaries
        let rowStart = Math.min(startRow, endRow);
        let rowEnd = Math.max(startRow, endRow);
        let colStart = Math.min(startCol, endCol);
        let colEnd = Math.max(startCol, endCol);

        // Iterate through the range and select the cells
        for (let x = rowStart; x <= rowEnd; x++) {
            let row = gTable.rows[x];
            for (let y = colStart; y <= colEnd; y++) {
                let cell = row.cells[y];
                selectCell(cell);
                numCells++;
            }
        }
        return numCells;
    }

    // Select a single cell.
    function selectCell(cell) {
        cell.classList.add('selected');
        return 1;
    }

    // Add event listener to the table that will
    // force tab selection to be go down the rows
    // rather than across the columns.
    gTable.addEventListener('keydown', tableHandleKeydown);

    // Event listener: single click.
    gTable.addEventListener('click', function(event) {
        const cell = event.target;

        // Check if the clicked element is a table cell
        // that is not in the header or the first column
        if (cell.tagName.toLowerCase() === 'td') {
            // Get the row and column index of the cell
            let rowIndex = cell.parentElement.rowIndex;
            let colIndex = cell.cellIndex;

            // If we're not in the first row or column, allow selection
            if (rowIndex > 0 && colIndex > 0) {
                // Start a timer for the single-click action
                // so that we don't mix it up with a double-click
                // action
                const clickTimer = setTimeout(() => {
                    // Single-click

                    // Shift
                    if (event.shiftKey && gLastSelectedCell) {
                        gNumCellsSelected += selectRange(gLastSelectedCell, cell);
                        // Don't update the last selected cell
                        // so that the pivot-point remains
                        // the same for future shift-clicks
                    // Ctrl
                    } else if (event.ctrlKey || event.metaKey) {
                        gNumCellsSelected += toggleCellSelection(cell);
                        // Update the last selected cell
                        gLastSelectedCell = cell;
                    // Normal
                    } else {
                        if (gNumCellsSelected > 1) {
                            // In multi-select mode: clear the selection
                            // and select just this cell
                            gNumCellsSelected = clearSelection();
                            gNumCellsSelected += toggleCellSelection(cell);
                            // Update the last selected cell
                            gLastSelectedCell = cell;
                        } else {
                            // In single-select mode: toggle any
                            // previous cell and, if we're not on the
                            // same cell, toggle this cell
                            if (gLastSelectedCell) {
                                gNumCellsSelected += toggleCellSelection(gLastSelectedCell);
                            }
                            if (gLastSelectedCell != cell) {
                                gNumCellsSelected += toggleCellSelection(cell);
                                if (gNumCellsSelected > 0) {
                                    gLastSelectedCell = cell;
                                }
                            } else {
                                // Clear the last selected cell or
                                // we will have a spurious pivot
                                // point the next time we enter
                                // multi-select mode
                                gLastSelectedCell = null;
                            }
                        }
                    }
                }, delayMs);
                clickTimerList.push(clickTimer);
            }
        }
    });

    // Event listener, double-click: do something with the selected cells
    gTable.addEventListener('dblclick', function(event) {
        const cell = event.target;
        // Cancel any single-click timers
        clickTimerList.forEach(timerId => clearTimeout(timerId));
        clickTimerList = [];
        if (gNumCellsSelected > 0) {
            // Do the schedule change
            scheduleChange();
        }
    });
});

/* ----------------------------------------------------------------
 * WEEKLY SCHEDULE STUFF: DATA RETRIEVAL AND TABLE DISPLAY
 * -------------------------------------------------------------- */

// Dynamic table creator, following the pattern here:
// https://jsfiddle.net/onury/kBQdS/.
const gDynamicTable = (function() {
    let _tableId, _table, _columnTitles, _rowTitles, _defaultText;

    // Build a row.  Data can be a list of strings or it
    // can be a ilst of objects with "class" and "contents"
    // members.
    function _buildRow(rowTitle, data) {
        const columnPrefix = '<td valign="top" align="center"';
        const columnPostfix = '</td>';
        let ariaLabelPrefix = null;
        let row = '<tr>';
        if (rowTitle) {
            row += columnPrefix + '>' + rowTitle + columnPostfix;
            if (_columnTitles) {
                ariaLabelPrefix = 'aria-label="';
            }
        }
        if (data) {
            data.forEach(function(column, index) {
                let cellClass = '';
                let contents = column;
                let tabIndex = ''
                if (column['class']) {
                    cellClass = ' class="' + column.class + '"';
                    // Adding tabIndex="0" allows the cell to be tabbed-to
                    tabIndex = ' tabindex="0"'
                    contents = column.contents;
                }
                let ariaLabel = '';
                if (ariaLabelPrefix != null) {
                    ariaLabel = ariaLabelPrefix + rowTitle + ' on ' + _columnTitles[index] + '"';
                }
                row += columnPrefix + cellClass + ariaLabel + tabIndex + '>' + contents + columnPostfix;
            });
        }
        row += '</tr>';
        return row;
    }

    // Make a colour class object for all of the cells.
    // An entry in the object may be accessed by using a key
    // that is the column title and the row title concatanated.
    function _cellColour(data) {
        let colour = {};
        if (data && _rowTitles && _columnTitles) {
            // At the start of the week, motors and lights are on
            let motorsOff = false;
            let lightsOff = false;
            _columnTitles.forEach(function(columnTitle) {
                let dayData = data[columnTitle];
                _rowTitles.forEach(function(rowTitle) {
                    if (dayData) {
                        let cellData = dayData[rowTitle];
                        if (cellData) {
                            cellData.forEach(function(item) {
                                if (item.switchType === 'off') {
                                    if (item.thing === 'motors') {
                                       motorsOff = true;
                                    } else if (item.thing === 'lights') {
                                       lightsOff = true;
                                    }
                                } else if (item.switchType === 'on') {
                                    if (item.thing === 'motors') {
                                       motorsOff = false;
                                    } else if (item.thing === 'lights') {
                                       lightsOff = false;
                                    }
                                }
                            })
                        }
                    }
                    let cell = 'cell-on';
                    if (motorsOff || lightsOff) {
                        if (motorsOff && lightsOff) {
                            cell = 'cell-off';
                        } else if (motorsOff) {
                            cell = 'cell-motors-off-lights-on';
                        } else {
                            cell = 'cell-motors-on-lights-off';
                        }
                    }
                    colour[columnTitle + rowTitle] = cell;
                })
            })
        }
        return colour;
    }

    // Build the headers of the table.
    function _setColumnTitles(columnTitles) {
        if (!columnTitles) {
            columnTitles = _columnTitles;
        }
        const h = _buildRow(' ', columnTitles);
        if (_table.children('thead').length < 1) {
            _table.prepend('<thead></thead>');
        }
        _table.children('thead').html(h);
    }

    // Display something when we have no data to populate the table.
    function _setNoItemsInfo() {
        if (_table.length > 0) {
            const colspan = _columnTitles != null && _columnTitles.length > 0 ?
                            'colspan="' + _columnTitles.length + '"' : '';
            const content = '<tr class="no-items"><td ' + colspan +
                            ' valign="top" align="center">' + 
                            _defaultText + '</td></tr>';
            if (_table.children('tbody').length > 0) {
                _table.children('tbody').html(content);
            } else {
                _table.append('<tbody>' + content + '</tbody>');
            }
        }
    }

    // Remove the "no data to display" thingy.
    function _removeNoItemsInfo() {
        const c = _table.children('tbody').children('tr');
        if (c.length == 1 && c.hasClass('no-items')) {
            _table.children('tbody').empty();
        }
    }

    return {
        // Configure the dynamic table.
        cfg: function(tableId, columnTitles, rowTitles, defaultText) {
            _tableId = tableId;
            _table = $('#' + tableId);
            _setColumnTitles(columnTitles);
            _columnTitles = columnTitles || null;
            _rowTitles = rowTitles || null;
            _defaultText = defaultText || 'No data';
            _setNoItemsInfo();
            return this;
        },
        // Load data into the dynamic table.
        // Data should be an object containing an entry for each
        // columnTitle; if there is data at one of those column
        // titles it will be in a list attached as an object
        // whose name matches one of our rowTitles.
        set: function(data) {
            if (_table.length > 0) {
                _setColumnTitles();
                _removeNoItemsInfo();
                if (data && _rowTitles && _columnTitles) {
                    // We want to colour the cells based on the state
                    // of the motors and the lights but we can't do
                    // that as we go as the order of the table is
                    // wrong: get a matrix of cell colours that we 
                    // can apply as we go.
                    const cellColours = _cellColour(data);
                    _rowTitles.forEach(function(rowTitle) {
                        let column = [];
                        _columnTitles.forEach(function(columnTitle) {
                            let contents = '';
                            let dayData = data[columnTitle];
                            if (dayData) {
                                let cellData = dayData[rowTitle];
                                if (cellData) {
                                    cellData.forEach(function(item, index) {
                                        if (index > 0) {
                                            contents += ', ';
                                        }
                                        contents = item.thing + ' ' + item.switchType;
                                    });
                                }
                            }
                            column.push({"contents": contents,
                                         "class": cellColours[columnTitle + rowTitle]});
                        });
                        const row = _buildRow(rowTitle, column);
                        _table.children('tbody')['append'](row);
                    });
                } else {
                    _setNoItemsInfo();
                }
            }
            return this;
        },
        // Clear the table body.
        clear: function() {
            _setNoItemsInfo();
            return this;
        }
    };
}());

// Convert a string of the form HH:MM:SS into seconds.
function timeStrToSeconds(timeStr) {
    return parseInt(timeStr.substring(0, 2), 10) * 60 * 60 +
           parseInt(timeStr.substring(3, 5), 10) * 60 +
           parseInt(timeStr.substring(7, 9), 10);
}

// Fetch a data file from the server.
async function dataFetch(file) {
    let json = {};

    try {
        let object = await fetch(file);
        let rawContents = await object.text();
        json = await JSON.parse(rawContents);
    } catch {
        console.log("unable to fetch " + file);
    }

    return json;
}

// Populate the weekly schedule table.
function loadTable(cfgData) {
    // The days of the week, which are our column titles and will be the
    // same as the days of the week returned in the configuration file
    // from the server (ignoring case)
    const dayList = ['Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday', 'Sunday'];
    // The HH:MM:SS of the day, which are our row titles
    const timeStrList = ['00:00:00', '01:00:00', '02:00:00', '03:00:00', '04:00:00', '05:00:00',
                         '06:00:00', '07:00:00', '08:00:00', '09:00:00', '10:00:00', '11:00:00',
                         '12:00:00', '13:00:00', '14:00:00', '15:00:00', '16:00:00', '17:00:00',
                         '18:00:00', '19:00:00', '20:00:00', '21:00:00', '22:00:00', '23:00:00'];
    // How long each of the times above covers
    const durationSeconds = 60 * 60;
    // The things we want to show in the table; these will be in the configuration
    // data we receive from the server
    const thingList = ['motors', 'lights'];
    // The types of operations on the things that we want to show in the table,
    // which will again be in the data we receive from the server
    const switchTypeList = ['off', 'on'];

    // Configure the table
    const weekTable = gDynamicTable.cfg('week', dayList, timeStrList, 'Loading...');

    // The form of this data is as described at the top of w_cfg.h;
    // turn it into a block of weekly data that can be passed to
    // the table
    const weekData = cfgData['week'];
    let daysObject = {};
    if (weekData) {
        dayList.forEach(function(day) {
            let timesObject = {};
            const dayData = weekData[day.toLowerCase()];
            if (dayData) {
                // Check for all of our time windows
                timeStrList.forEach(function(timeStr) {
                    let happenings = [];
                    // Check for all of our things
                    thingList.forEach(function(thing) {
                        // Check for all of our switch types
                        switchTypeList.forEach(function(switchType) {
                            // See if there is a list of times for that switch type
                            let timeList = [];
                            try {
                                timeList = dayData[thing][switchType];
                            } catch {
                                // There was not
                            }
                            if (timeList && (timeList.length > 0)) {
                                timeList.forEach(function(switchTimeStr) {
                                    // We have timeStr (the row title), which is "HH:MM:SS",
                                    // and, in switchTimeStr (also "HH:MM:SS"), we have a
                                    // time at which a switch type (off or on) occurs for a
                                    // thing (motors or lights); if switchTimeStr is within
                                    // the/ time window or timeStr, add an object to the
                                    // list describing what happens then
                                    const switchTime = timeStrToSeconds(switchTimeStr);
                                    const time = timeStrToSeconds(timeStr);
                                    if ((switchTime >= time) && (switchTime < time + durationSeconds)) {
                                        happenings.push({'time': switchTimeStr, thing: thing, switchType: switchType});
                                    }
                                });
                            }
                        });
                    });
                    // If there were any happenings, make a time object with [a copy of]
                    // the list of happenings and add it to the timesObject we were
                    // passed
                    if (happenings.length > 0) {
                        timesObject[timeStr] = Array.from(happenings);
                    }
                });
                // Add the day to the days object, if anything happened then
                if (timesObject) {
                    daysObject[day] = timesObject;
                }
            }
        });
    }

    // daysObject contains all of the days from dayList[], to each of which
    // is attached a set of objects named after the entries of timeStrList[]
    // if something is scheduled to happen in that time window; pass
    // this into our table.
    weekTable.set(daysObject);
}

// Wait for the asynchronous function to complete before continuing
// to load the weekly schedule table.
(async () => {
    // Fetch the configuration data from the server
    const cfgData = await dataFetch('watchdog.cfg');
    loadTable(cfgData);
    // Store a zeroeth selected state for table undo/redo
    saveSelectionState(true);
})();

// End of file
