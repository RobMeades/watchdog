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

'use strict';

/* ----------------------------------------------------------------
 * MISC
 * -------------------------------------------------------------- */

// The name of the configuration file on the server, which contains
// the weekly schedule.
let gCfgFileName = 'watchdog.cfg';

// Track the state of the control key.
let gKeyIsPressedCtrl = false;

// Event listener: for CTRL-key handling.
document.addEventListener('keyup', function(event) {
    if (event.key === 'Control' || event.key === 'Meta') {
        gKeyIsPressedCtrl = false;
    }
});

// Event listener: for undo/redo keys on the whole document,
// done in a slightly peculiar way as the browser's own
// undo functionality interferes with the key-presses we
// receive.
document.addEventListener('keydown', function(event) {
    if (event.key === 'Control' || event.key === 'Meta') {
        gKeyIsPressedCtrl = true;
        // Ctrl-Z (undo)
    } else if (gKeyIsPressedCtrl && (event.key === 'z' || event.key === 'Z')) {
        event.preventDefault();
        historyUndo();
        // Ctrl-Y (redo)
    } else if (gKeyIsPressedCtrl && (event.key === 'y' || event.key === 'Y')) {
        event.preventDefault();
        historyRedo();
    }
});

/* ----------------------------------------------------------------
 * HLS VIDEO PLAYING STUFF
 * -------------------------------------------------------------- */

// Grab the element IDs
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
 * WEEKLY SCHEDULE STUFF: THE SCHEDULE CHANGE DIALOG
 * -------------------------------------------------------------- */

// Grab the IDs of the elements of the schedule change dialogue box.
const gScheduleChangeDialog = document.getElementById('schedule-change');
const gScheduleChangeDialogCloseBtn = document.getElementById('dialog-close-btn');
const gScheduleChangeSubmitBtn = document.getElementById('schedule-change-submit-btn');
const gScheduleChangeRadioMotors = document.getElementById('radio-motors');
const gScheduleChangeRadioLights = document.getElementById('radio-lights');

// Engage the schedule-change modal dialog box.
function scheduleChangeDialog() {
    const radioButtons = document.querySelectorAll('input[type="radio"]');
    // Uncheck the radio buttons before displaying them,
    // as a kind of reset mechanism in case the user
    // ticks one they didn't intend to
    radioButtons.forEach(radio => {
        radio.checked = false;
    });

    // Show the dialog box; event listeners will close it
    gScheduleChangeDialog.style.display = 'flex';
    // Since the dialog is a div, we need to move focus manually
    gTableLastFocusedElement = document.activeElement;
    gScheduleChangeDialog.focus();
}

// Close the schedule change dialog: called by event listeners.
function scheduleChangeDialogClose() {
    gScheduleChangeDialog.style.display = 'none';
    if (gTableLastFocusedElement) {
        gTableLastFocusedElement.focus();
    }
}

// Event listener: close the schedule-change dialogue box
// when the close button is clicked.
gScheduleChangeDialogCloseBtn.addEventListener('click', () => {
    scheduleChangeDialogClose();
});

// Event listener: close the schedule-change dialog when
// clicking outside the dialogue.
window.addEventListener('click', (event) => {
    if (event.target === gScheduleChangeDialog) {
        scheduleChangeDialogClose();
    }
});

// Event listener: close the schedule-change dialogue box
// if the escape key is pressed.
gScheduleChangeDialog.addEventListener('keydown', (event) => {
    if (event.key === 'Escape') {
        scheduleChangeDialogClose();
    }
});

// Event listener: schedule-change form submission.
// Note: this needs to be marked as async since it
// ends up calling fetch() to write the data to the
// server.
gScheduleChangeSubmitBtn.addEventListener('click', async () => {
    // Remove the dialog box
    gScheduleChangeDialog.style.display = 'none';

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
        // Display an "are you sure" dialog box using the browser's
        // native confirm() mechanism
        isConfirmed = confirm('Set ' + notification + ': are you sure?');
    } else {
        notification = 'Nothing to do';
    }

    if (isConfirmed) {
        // Get the selected cells and update the table
        const selectedCells = gTable.querySelectorAll('td.selected');
        gDynamicTable.setMotorsLights(selectedCells, selectedMotors, selectedLights);
        let data = {};
        gDynamicTable.getMotorsLights(data);
        // Send the data to the server
        notification = null;
        if (Object.keys(data).length !== 0) {
            let success = false;
            try {
                const response = await dataFetchPost(data, gCfgFileName);
                if (response.ok) {
                    notification = 'Changes successfully written';
                    success = true;
                } else {
                    notification = 'Error from server "' + response.status + '"';
                }
            } catch (error) {
                notification = 'Error "' + error + '" sending data';
            }
 
            if (!success) {
                notification += '; the table is now out of sync with the server,' +
                                ' please refresh this page'
            }
        } else {
            notification = 'Internal error: no data read from the table';
        }

        // Clear selection and selection state history,
        // adding a new zero state
        tableClearSelection();
        historyStateClear();
        historySaveState(true);
        gTableLastSelectedCell = null;
        gTableNumCellsSelected = 0;
    } else {
        notification = 'Cancelled';
    }

    if (gTableLastFocusedElement) {
        gTableLastFocusedElement.focus();
    }

    if (notification != null) {
        showNotification(notification);
    }
});

/* ----------------------------------------------------------------
 * WEEKLY SCHEDULE STUFF: NAVIGATING THE TABLE
 * -------------------------------------------------------------- */

// Variables related to navigating the table
let gTable = document.getElementById('week');
let gTableLastSelectedCell = null;
let gTableNumCellsSelected = 0;
let gTableLastFocusedElement;

// Select a single cell.
function tableSelectCell(cell) {
    cell.classList.add('selected');
    return 1;
}

// Select a range of cells.
function tableSelectRange(startCell, endCell) {
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
            tableSelectCell(cell);
            numCells++;
        }
    }
    return numCells;
}

// Toggle selection of the given cell.
function tableToggleCellSelection(cell) {
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

// Clear all selections.
function tableClearSelection() {
    let selectedCells = gTable.querySelectorAll('td.selected');
    selectedCells.forEach(cell => cell.classList.remove('selected'));
    return 0;
}

// A function which is attached to the table via an event listener
// and (a) makes the tab ordering go down the columns rather than
// across the rows, (b) allows arrow keys to be used for navigation
// and (c) handles <enter>/<space> to select a cell and CTRL with
// those to submit a change.
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
        } else if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault();
            if (gKeyIsPressedCtrl) {
                if (gTableNumCellsSelected > 0) {
                    // Do the schedule change
                    scheduleChangeDialog();
                }
            } else {
                if (event.shiftKey && gTableLastSelectedCell &&
                    gTableLastSelectedCell != event.target) {
                    gTableNumCellsSelected +=  tableSelectRange(gTableLastSelectedCell,
                                                                event.target);
                } else {
                    gTableNumCellsSelected += tableToggleCellSelection(event.target);
                }
                // Update the last selected cell
                gTableLastSelectedCell = event.target;
                historySaveState();
            }
        }
    }
}

// Event listener: handle cells of the table being selected.
document.addEventListener('DOMContentLoaded', function() {
    let clickTimerList = [];
    let delayMs = 300;
    let isDragging = false;
    let startCell = null;

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
                if (!event.shiftKey) {
                    gTableNumCellsSelected = tableClearSelection();
                    gTableLastSelectedCell = null;
                }
                gTableNumCellsSelected += tableSelectRange(startCell, cell);
            }
        }
    }

    // No more dragging; save state.
    function handleMouseUp() {
        isDragging = false;
        historySaveState();
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
                    if (event.shiftKey) {
                        if (gTableLastSelectedCell) {
                            gTableNumCellsSelected += tableSelectRange(gTableLastSelectedCell, cell);
                        } else {
                            gTableNumCellsSelected += tableToggleCellSelection(cell);
                        }
                        // Don't update the last selected cell
                        // so that the pivot-point remains
                        // the same for future shift-clicks
                    // Ctrl
                    } else if (event.ctrlKey || event.metaKey) {
                        gTableNumCellsSelected += tableToggleCellSelection(cell);
                        // Update the last selected cell
                        gTableLastSelectedCell = cell;
                    // Normal
                    } else {
                        if (gTableNumCellsSelected > 1) {
                            // In multi-select mode: clear the selection
                            // and select just this cell
                            gTableNumCellsSelected = tableClearSelection();
                            gTableNumCellsSelected += tableToggleCellSelection(cell);
                            // Update the last selected cell
                            gTableLastSelectedCell = cell;
                        } else {
                            // In single-select mode: toggle any
                            // previous cell and, if we're not on the
                            // same cell, toggle this cell
                            if (gTableLastSelectedCell) {
                                gTableNumCellsSelected += tableToggleCellSelection(gTableLastSelectedCell);
                            }
                            if (gTableLastSelectedCell != cell) {
                                gTableNumCellsSelected += tableToggleCellSelection(cell);
                                if (gTableNumCellsSelected > 0) {
                                    gTableLastSelectedCell = cell;
                                }
                            } else {
                                // Clear the last selected cell or
                                // we will have a spurious pivot
                                // point the next time we enter
                                // multi-select mode
                                gTableLastSelectedCell = null;
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
        if (gTableNumCellsSelected > 0) {
            // Do the schedule change
            scheduleChangeDialog();
        }
    });
});

/* ----------------------------------------------------------------
 * WEEKLY SCHEDULE STUFF: UNDO/REDO HISTORY FOR THE TABLE
 * -------------------------------------------------------------- */

// Global record of history.
let gHistory = [];
let gHistoryStateIndex = -1;

// Save the currently selected table cells for undo/redo.
function historySaveState(zero = false) {
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
    if (gHistoryStateIndex < gHistory.length - 1) {
        gHistory.splice(gHistoryStateIndex + 1);
    }

    // Add the new state to the history
    gHistory.push(selectedState);
    gHistoryStateIndex = gHistory.length - 1;
}

// Retore the state of table cell selection, used by table undo/redo.
function historyRestoreSelectionState() {
    // Clear the current selection
    tableClearSelection();

    // Restore the selection state from history
    const selectedState = gHistory[gHistoryStateIndex];
    gTableLastSelectedCell = null;
    gTableNumCellsSelected = 0;
    if (selectedState) {
        selectedState.forEach(({ row, col }) => {
            const cell = gTable.rows[row].cells[col];
            cell.classList.add('selected');
            gTableNumCellsSelected++;
        });
    }
}

// Clear selection state history.
function historyStateClear() {
    gHistory = [];
    gHistoryStateIndex = -1;
}

// Undo table cell selection.
function historyUndo() {
    if (gHistoryStateIndex > 0) {
        gHistoryStateIndex--; // Move to the previous state
        historyRestoreSelectionState();
    }
}

// Redo table cell selection.
function historyRedo() {
    if (gHistoryStateIndex < gHistory.length - 1) {
        gHistoryStateIndex++; // Move to the next state
        historyRestoreSelectionState();
    }
}

/* ----------------------------------------------------------------
 * WEEKLY SCHEDULE STUFF: DATA RETRIEVAL AND DISPLAY OF THE TABLE
 * -------------------------------------------------------------- */

// Dynamic table creator, following the pattern here:
// https://jsfiddle.net/onury/kBQdS/.  This should still
// work with genericish data but has neem heavily enhanced to
// work purely with the exact data from here, vix "motors" and
// "lights" being switched "on" and "off" across a weekly
// schedule.
const gDynamicTable = (function() {
    let _tableId, _table, _columnTitles, _rowTitles, _defaultText;

    // Build a row.  Data can be a list of strings or it
    // can be a ilst of objects with "classList" and "contents"
    // members.
    function _buildRow(rowTitle, data) {
        const columnPrefix = '<td valign="top" align="center"';
        const columnPostfix = '</td>';
        let ariaLabelPrefix = null;
        let row = '<tr>';
        if (rowTitle) {
            row += columnPrefix + '>' + rowTitle + columnPostfix;
            if (_columnTitles) {
                ariaLabelPrefix = ' aria-label="';
            }
        }
        if (data) {
            data.forEach(function(column, index) {
                let classes = '';
                let contents = column;
                let tabIndex = ''
                if (column['classList']) {
                    classes = ' class ="';
                    column['classList'].forEach(function(_class, index) {
                        if (index > 0) {
                            classes += ' ';
                        }
                        classes += _class;
                    });
                    classes += '"';
                    // Adding tabIndex="0" allows the cell to be tabbed-to
                    tabIndex = ' tabindex="0"'
                    contents = column.contents;
                }
                let ariaLabel = '';
                if (ariaLabelPrefix != null) {
                    ariaLabel = ariaLabelPrefix + rowTitle + ' on ' + _columnTitles[index] + '"';
                }
                row += columnPrefix + classes + ariaLabel + tabIndex + '>' + contents + columnPostfix;
            });
        }
        row += '</tr>';
        return row;
    }

    // There is a nice built in JS function for arrays, filter(),
    // which can be used to remove entries.  However, it does
    // not work on cell classLists, which are not arrays at all
    // but are DOMTokenLists which are "array like" but don't
    // support filter() or push().  Hence this function, which does
    // the filtering stuff for either, taking the data type into
    // account.
    function _listUpdate(sourceList, removeList = null, addList = null) {
        let newList = sourceList;
        if (newList) {
            if (Array.isArray(newList)) {
                // It is an array, do array things
                if (removeList) {
                    removeList.forEach(function(remove) {
                        newList = newList.filter(value => value !== remove);
                    });
                }
                if (addList) {
                    addList.forEach(function(add) {
                        newList.push(add);
                    });
                }
            } else {
                // Assume it is a DOMTokenList
                if (removeList) {
                    removeList.forEach(function(remove) {
                        newList.remove(remove);
                    });
                }
                if (addList) {
                    addList.forEach(function(add) {
                        newList.add(add);
                    });
                }
            }
        }
        return newList;
    }

    // Take a list of classes and filter them according to whether
    // the thing is "motors" or "lights" and the switchType "off"
    // or "on".
    function _classListFilterMotorsLights(classList, thing, switchType) {
        let removeList = [];
        let addList = [];
        if (switchType === 'off') {
            // Reemove cell-on, as all are no longer on
            removeList.push('cell-on');
            if (thing === 'motors') {
                // Remove any previous motors off, to prevent duplicates,
                // and add it
                removeList.push('cell-motors-off');
                addList.push('cell-motors-off');
            } else if (thing === 'lights') {
                // Remove any previous motors off, to prevent duplicates,
                // and add it
                removeList.push('cell-lights-off');
                addList.push('cell-lights-off');
            }
        } else if (switchType === 'on') {
            if (thing === 'motors') {
                // Remove the motors off class from the list
                removeList.push('cell-motors-off');
            } else if (thing === 'lights') {
                removeList.push('cell-lights-off');
            }
        }
        return _listUpdate(classList, removeList, addList);
    }

    // Finalise a class list for "motors" and "lights";
    // classList can be an array or a DOMTokenList.
    function _classListCompleteMotorsLights(classList) {
        let newList = classList;
        let thingOffCount = 0;
        if (newList) {
            newList.forEach(function(thisClass) {
                if (thisClass === 'cell-motors-off' ||
                    thisClass === 'cell-lights-off') {
                    thingOffCount++;
                }
            });
            if (thingOffCount == 0) {
                // All off, so set 'cell-on'
                if (Array.isArray(newList)) {
                    // An array, do array things
                    newList.push('cell-on');
                } else {
                    // Assume it is a DOMTokenList
                    newList.add('cell-on');
                }
            }
        }
        return newList;
    }

    // Make a class object for all of the cells.
    // An entry in the object, which will be a list of classes
    // that must be applied to the cell, may be accessed by using a key
    // that is the column title and the row title concatanated.
    function _cellClasses(data) {
        let classObject = {};
        if (data && _rowTitles && _columnTitles) {
            // TODO: deal with starting condition
            let classList = [];
            classList.push('cell-on');
            _columnTitles.forEach(function(columnTitle) {
                let dayData = data[columnTitle];
                _rowTitles.forEach(function(rowTitle) {
                    if (dayData) {
                        let cellData = dayData[rowTitle];
                        if (cellData) {
                            cellData.forEach(function(item) {
                                classList = _classListFilterMotorsLights(classList, item.thing,
                                                                         item.switchType);
                            })
                            classList = _classListCompleteMotorsLights(classList);
                        }
                    }
                    if (classList.length > 0) {
                        // Sort the array before pushing so that it can be used in comparisons
                        classObject[columnTitle + rowTitle] = Array.from(classList.sort());
                    }
                })
            })
        }
        return classObject;
    }

    // Return the appropriate cell contents string given the
    // classes attached (a string with space separated
    // classes).
    function _cellContentsFromClassesMotorsLights(classes) {
        let contents = '';
        if (classes) {
            if (classes.includes('cell-on')) {
                contents += 'normal';
            } else {
                if (classes.includes('cell-motors-off')) {
                    contents += 'motors off';
                }
                if (classes.includes('cell-lights-off')) {
                    if (contents) {
                        contents += ', ';
                    }
                    contents += 'lights off';
                }
            }
        }
        return contents;
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
                    // wrong: get a list of cell classes that we 
                    // can apply as we go.
                    const classes = _cellClasses(data);
                    let previousClasses = null;
                    let lastClassList = [];
                    _rowTitles.forEach(function(rowTitle) {
                        let column = [];
                        _columnTitles.forEach(function(columnTitle) {
                            let contents = '';
                            let thisClasses = previousClasses;
                            let classList = classes[columnTitle + rowTitle];
                            if (classList) {
                                thisClasses = JSON.stringify(classList);
                                lastClassList =  Array.from(classList);
                            }
                            if (thisClasses) {
                                // If there are classLists for this data,
                                // we can display helpful text on that basis,
                                // doing it only when the state changes
                                if (thisClasses) {
                                    contents += _cellContentsFromClassesMotorsLights(thisClasses);
                                }
                                column.push({"contents": contents,
                                             "classList": lastClassList});
                                if (thisClasses) {
                                    previousClasses = thisClasses;
                                }
                            } else {
                                // No classList, just use the data directly
                                let dayData = data[columnTitle];
                                if (dayData) {
                                    let cellData = dayData[rowTitle];
                                    if (cellData) {
                                        cellData.forEach(function(item, index) {
                                            if (index > 0) {
                                                contents += ', ';
                                            }
                                            contents += item.thing + ' ' + item.switchType;
                                        });
                                    }
                                }
                                column.push(contents);
                            }
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
        // Set a sub-set of cells specifically for motors/lights.
        // The values of motors and lights may be null or, if not
        // null, will have a value element that is "on" or "off".
        setMotorsLights: function(cellList, motors, lights) {
            cellList.forEach(function(cell) {
                if (motors) {
                    cell.classList = _classListFilterMotorsLights(cell.classList,
                                                                  'motors',
                                                                  motors.value);
                }
                if (lights) {
                    cell.classList = _classListFilterMotorsLights(cell.classList,
                                                                  'lights',
                                                                  lights.value);
                }
                cell.classList = _classListCompleteMotorsLights(cell.classList);
                cell.innerHTML = _cellContentsFromClassesMotorsLights(cell.classList.value);
            });
            return this;
        },
        // Get the motors/lights data out of the table, doing the
        // opposite of set().
        getMotorsLights: function(data) {
            // Get the rows and columns from the DOM
            let trList = gTable.querySelectorAll('tr');
            if (trList.length > 0 && _rowTitles) {
                let rowList = [];
                trList.forEach(function(tr) {
                    rowList.push(Array.from(tr.querySelectorAll('td')));
                });
                // Make a list of the days (ignoring the empty column)
                // in lower case
                let dayList = [];
                if (rowList.length > 0) {
                    rowList[0].forEach(function(column, index) {
                        if (index > 0) {
                            dayList.push(column.innerHTML.toLowerCase())
                        }
                    });
                }
                // Work down the rows of each day looking for changes
                // in the classes that represent motors/lights state
                // TODO: resolve starting condition
                let motorsOff = false;
                let lightsOff = false;
                let dayData = {};
                dayList.forEach(function(day, columnIndex) {
                    // Increment the columnIndex so that we can use
                    // it to index into the table-based arrays
                    columnIndex++;
                    let motorsOffTimeList = [];
                    let motorsOnTimeList = [];
                    let lightsOffTimeList = [];
                    let lightsOnTimeList = [];
                    _rowTitles.forEach(function(rowTitle, rowIndex) {
                        // Increment row index for the same reason
                        // as column index was incremented
                        rowIndex++;
                        if (rowIndex < rowList.length) {
                            let columnList = rowList[rowIndex];
                            if (columnIndex < columnList.length) {
                                // Add to our lists for the day based
                                // on the classes attached to the cell
                                let cell = columnList[columnIndex];
                                let classes = cell.classList.value;
                                if (motorsOff) {
                                    if (classes.includes('cell-on')) {
                                        motorsOnTimeList.push(rowTitle);
                                        motorsOff = false;
                                    }
                                } else {
                                    if (classes.includes('cell-motors-off')) {
                                        motorsOffTimeList.push(rowTitle);
                                        motorsOff = true;
                                    }
                                }
                                if (lightsOff) {
                                    if (classes.includes('cell-on')) {
                                        lightsOnTimeList.push(rowTitle);
                                        lightsOff = false;
                                    }
                                } else {
                                    if (classes.includes('cell-lights-off')) {
                                        lightsOffTimeList.push(rowTitle);
                                        lightsOff = true;
                                    }
                                }
                            }
                        }
                    });
                    // We now have the on and off times for the day;
                    // put them into the dayData object.
                    let switchType = {};
                    let things = {};
                    if (motorsOnTimeList.length > 0) {
                        switchType['on'] = Array.from(motorsOnTimeList);
                    }
                    if (motorsOffTimeList.length > 0) {
                        switchType['off'] = Array.from(motorsOffTimeList);
                    }
                    if (Object.keys(switchType).length !== 0) {
                        things['motors'] = switchType;
                    }
                    switchType = {};
                    if (lightsOnTimeList.length > 0) {
                        switchType['on'] = Array.from(lightsOnTimeList);
                    }
                    if (lightsOffTimeList.length > 0) {
                        switchType['off'] = Array.from(lightsOffTimeList);
                    }
                    if (Object.keys(switchType).length !== 0) {
                        things['lights'] = switchType;
                    }
                    if (Object.keys(things).length !== 0) {
                        dayData[day] = things;
                    }
                });
                // All changes of motors/lights should now
                // be in dayData: attach it to data as a week.
                if (Object.keys(dayData).length !== 0) {
                    data['week'] = dayData;
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
async function dataFetchGet(file) {
    let json = {};

    try {
        let object = await fetch(file);
        let rawContents = await object.text();
        json = await JSON.parse(rawContents);
    } catch (error) {
        console.log('unable to fetch ' + file + ', error "' + error + '"');
    }

    return json;
}

// Post JSON data to a file on the server.
async function dataFetchPost(json, file) {
    const headers = new Headers();
    headers.append("Content-Type", "application/json");

    try {
        const response = await fetch(file, {
            method: "POST",
            body: json,
            headers: headers,
        });
        return response; // Return the response object
    } catch (error) {
        console.log('Unable to post JSON data to ' + file + ', error "' + error + '"');
        throw error; // Throw the error to the caller
    }
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
                if (Object.keys(timesObject).length !== 0) {
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
    const cfgData = await dataFetchGet(gCfgFileName);
    loadTable(cfgData);
    // Store a zeroeth selected state for table undo/redo
    historySaveState(true);
})();

// End of file
