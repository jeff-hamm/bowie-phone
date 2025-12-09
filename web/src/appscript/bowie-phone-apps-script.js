/**
 * Google Apps Script for Bowie Phone Sequence Manager
 * Provides bidirectional sync between web UI and Google Sheets
 * Supports audio file upload to Google Drive
 * 
 * SETUP INSTRUCTIONS:
 * 1. Open Google Apps Script (script.google.com)
 * 2. Create new project
 * 3. Replace Code.gs with this content
 * 4. Create bowie-phone-apps-script.local.js for your real config values
 * 5. Deploy as web app with "Anyone" access
 * 6. Copy the web app URL to your frontend config
 * 
 * SHEET STRUCTURE:
 * Your Google Sheet should have these columns:
 * | Name | Number | Link |
 */

// Default configuration - DO NOT put real IDs here
// Create bowie-phone-apps-script.local.js with your real values
const DEFAULT_CONFIG = {
  sheetId: 'YOUR_SHEET_ID_HERE',
  gid: 'YOUR_SHEET_GID_HERE',
  sheetName: null, // Auto-derived from GID
  driveFolderId: 'YOUR_DRIVE_FOLDER_ID'
};

/**
 * Get configuration from request or use defaults
 */
function getConfig(requestData = {}) {
  const config = { ...DEFAULT_CONFIG };
  
  // Apply local config overrides if present
  try {
    if (typeof getLocalConfig === 'function') {
      const local = getLocalConfig();
      Object.assign(config, local);
    }
  } catch (e) {
    // No local config, use defaults
  }
  
  // Override with request config
  if (requestData.config) {
    Object.assign(config, requestData.config);
  }
  
  // Derive sheet name from GID if not provided
  if (!config.sheetName) {
    config.sheetName = getSheetNameFromGid(config.sheetId, config.gid);
  }
  
  return config;
}

/**
 * Get sheet name from GID
 */
function getSheetNameFromGid(sheetId, gid) {
  try {
    const spreadsheet = SpreadsheetApp.openById(sheetId);
    const sheets = spreadsheet.getSheets();
    const gidNum = parseInt(gid);
    
    for (const sheet of sheets) {
      if (sheet.getSheetId() === gidNum) {
        return sheet.getName();
      }
    }
    
    // Return first sheet if no match
    return sheets[0]?.getName() || 'Sheet1';
  } catch (error) {
    console.error('Error getting sheet name:', error);
    return 'Sheet1';
  }
}

/**
 * Main POST handler
 */
function doPost(e) {
  const corsHeaders = {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
    'Access-Control-Max-Age': '86400'
  };
  
  try {
    const requestData = JSON.parse(e.postData?.contents || '{}');
    const action = requestData.action;
    const config = getConfig(requestData);
    
    let result;
    
    switch (action) {
      case 'getSequences':
        result = getSequences(config);
        break;
      case 'addSequence':
        result = addSequence(requestData, config);
        break;
      case 'updateSequence':
        result = updateSequence(requestData, config);
        break;
      case 'deleteSequence':
        result = deleteSequence(requestData.sequenceId, config);
        break;
      case 'uploadFile':
        result = uploadFileToGoogleDrive(requestData, config);
        break;
      default:
        result = { error: `Unknown action: ${action}` };
    }
    
    return createCorsResponse({ success: true, data: result }, corsHeaders);
    
  } catch (error) {
    console.error('POST error:', error);
    return createCorsResponse({ success: false, error: error.toString() }, corsHeaders);
  }
}

/**
 * GET handler for testing and simple reads
 */
function doGet(e) {
  const corsHeaders = {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
    'Access-Control-Max-Age': '86400'
  };
  
  try {
    const action = e.parameter.action || 'getSequences';
    const config = getConfig({
      config: {
        sheetId: e.parameter.sheetId,
        gid: e.parameter.gid
      }
    });
    
    let result;
    
    switch (action) {
      case 'getSequences':
        result = getSequences(config);
        break;
      case 'test':
        result = { status: 'OK', timestamp: new Date().toISOString() };
        break;
      default:
        result = { error: `Unknown action: ${action}` };
    }
    
    return createCorsResponse({ success: true, data: result }, corsHeaders);
    
  } catch (error) {
    console.error('GET error:', error);
    return createCorsResponse({ success: false, error: error.toString() }, corsHeaders);
  }
}

/**
 * Get all sequences from sheet
 */
function getSequences(config) {
  const sheet = SpreadsheetApp.openById(config.sheetId).getSheetByName(config.sheetName);
  const data = sheet.getDataRange().getValues();
  
  if (data.length <= 1) {
    return { sequences: [], headers: data[0] || [] };
  }
  
  const headers = data[0].map(h => h.toString().toLowerCase().trim());
  const sequences = [];
  
  // Find column indices
  const nameIdx = findColumnIndex(headers, ['name', 'description', 'title']);
  const numberIdx = findColumnIndex(headers, ['number', 'sequence', 'dial']);
  const linkIdx = findColumnIndex(headers, ['link', 'url', 'audio', 'path']);
  
  for (let i = 1; i < data.length; i++) {
    const row = data[i];
    
    const name = nameIdx >= 0 ? (row[nameIdx] || '').toString().trim() : '';
    const number = numberIdx >= 0 ? (row[numberIdx] || '').toString().trim() : '';
    const link = linkIdx >= 0 ? (row[linkIdx] || '').toString().trim() : '';
    
    // Skip empty rows
    if (!name && !number) continue;
    
    sequences.push({
      id: `row-${i + 1}`,
      name: name,
      number: number,
      link: link,
      rowIndex: i + 1
    });
  }
  
  return {
    sequences: sequences,
    headers: headers,
    config: config
  };
}

/**
 * Add a new sequence
 */
function addSequence(data, config) {
  const sheet = SpreadsheetApp.openById(config.sheetId).getSheetByName(config.sheetName);
  const headers = sheet.getRange(1, 1, 1, sheet.getLastColumn()).getValues()[0];
  const headerMap = headers.map(h => h.toString().toLowerCase().trim());
  
  const nameIdx = findColumnIndex(headerMap, ['name', 'description', 'title']);
  const numberIdx = findColumnIndex(headerMap, ['number', 'sequence', 'dial']);
  const linkIdx = findColumnIndex(headerMap, ['link', 'url', 'audio', 'path']);
  
  // Prepare new row
  const newRow = new Array(headers.length).fill('');
  
  if (nameIdx >= 0) newRow[nameIdx] = data.name || '';
  if (numberIdx >= 0) newRow[numberIdx] = data.number || '';
  if (linkIdx >= 0) newRow[linkIdx] = data.link || '';
  
  // Add the row
  const newRowIndex = sheet.getLastRow() + 1;
  sheet.getRange(newRowIndex, 1, 1, newRow.length).setValues([newRow]);
  
  return {
    success: true,
    sequenceId: `row-${newRowIndex}`,
    rowIndex: newRowIndex
  };
}

/**
 * Update an existing sequence
 */
function updateSequence(data, config) {
  const rowIndex = parseInt(data.sequenceId.replace('row-', ''));
  const sheet = SpreadsheetApp.openById(config.sheetId).getSheetByName(config.sheetName);
  const headers = sheet.getRange(1, 1, 1, sheet.getLastColumn()).getValues()[0];
  const headerMap = headers.map(h => h.toString().toLowerCase().trim());
  
  // Map field names to column indices
  const fieldMap = {
    name: findColumnIndex(headerMap, ['name', 'description', 'title']),
    number: findColumnIndex(headerMap, ['number', 'sequence', 'dial']),
    link: findColumnIndex(headerMap, ['link', 'url', 'audio', 'path'])
  };
  
  // Update each provided field
  for (const [field, value] of Object.entries(data)) {
    if (field === 'action' || field === 'sequenceId' || field === 'config') continue;
    
    const colIdx = fieldMap[field];
    if (colIdx >= 0) {
      sheet.getRange(rowIndex, colIdx + 1).setValue(value);
    }
  }
  
  return {
    success: true,
    sequenceId: data.sequenceId,
    rowIndex: rowIndex
  };
}

/**
 * Delete a sequence (remove the row)
 */
function deleteSequence(sequenceId, config) {
  const rowIndex = parseInt(sequenceId.replace('row-', ''));
  const sheet = SpreadsheetApp.openById(config.sheetId).getSheetByName(config.sheetName);
  
  sheet.deleteRow(rowIndex);
  
  return {
    success: true,
    deletedSequenceId: sequenceId
  };
}

/**
 * Upload audio file to Google Drive
 */
function uploadFileToGoogleDrive(data, config) {
  try {
    const { fileName, fileData, mimeType, driveFolder } = data;
    
    if (!fileName || !fileData || !mimeType) {
      throw new Error('Missing required: fileName, fileData, or mimeType');
    }
    
    // Decode base64
    const blob = Utilities.newBlob(
      Utilities.base64Decode(fileData),
      mimeType,
      fileName
    );
    
    // Get folder ID
    let folderId = config.driveFolderId;
    
    if (driveFolder && driveFolder.includes('folders/')) {
      folderId = driveFolder.match(/folders\/([a-zA-Z0-9-_]+)/)?.[1] || folderId;
    }
    
    if (!folderId || folderId === 'YOUR_DRIVE_FOLDER_ID') {
      throw new Error('Google Drive folder not configured');
    }
    
    const folder = DriveApp.getFolderById(folderId);
    
    // Create file with timestamp
    const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
    const uniqueFileName = `${timestamp}_${fileName}`;
    
    const file = folder.createFile(blob.setName(uniqueFileName));
    
    // Make publicly viewable
    file.setSharing(DriveApp.Access.ANYONE_WITH_LINK, DriveApp.Permission.VIEW);
    
    const fileUrl = file.getUrl();
    
    console.log(`File uploaded: ${uniqueFileName} -> ${fileUrl}`);
    
    return {
      success: true,
      fileUrl: fileUrl,
      fileName: uniqueFileName,
      fileId: file.getId()
    };
    
  } catch (error) {
    console.error('Upload error:', error);
    return {
      success: false,
      error: `Upload failed: ${error.toString()}`
    };
  }
}

/**
 * Helper: find column index by possible names
 */
function findColumnIndex(headers, possibleNames) {
  for (const name of possibleNames) {
    const idx = headers.indexOf(name.toLowerCase());
    if (idx >= 0) return idx;
  }
  return -1;
}

/**
 * Create CORS-enabled response
 */
function createCorsResponse(data, corsHeaders = {}) {
  const output = ContentService.createTextOutput(JSON.stringify(data))
    .setMimeType(ContentService.MimeType.JSON);
  
  // Note: Apps Script has limited header support, but we include them for documentation
  return output;
}

/**
 * Test function
 */
function testScript() {
  const config = getConfig();
  console.log('Config:', config);
  
  try {
    const result = getSequences(config);
    console.log('Sequences:', result.sequences.length);
    return { success: true, sequenceCount: result.sequences.length };
  } catch (error) {
    console.error('Test error:', error);
    return { success: false, error: error.toString() };
  }
}

/**
 * Export sequences as JSON (for phone firmware)
 */
function exportAsJSON() {
  const config = getConfig();
  const data = getSequences(config);
  
  const output = {};
  
  data.sequences.forEach(seq => {
    if (seq.number) {
      output[seq.number] = {
        description: seq.name || seq.number,
        type: guessType(seq.link),
        path: seq.link || ''
      };
    }
  });
  
  console.log('Exported JSON:', JSON.stringify(output, null, 2));
  return output;
}

/**
 * Helper: guess sequence type from link
 */
function guessType(link) {
  if (!link) return 'shortcut';
  
  const lower = link.toLowerCase();
  
  if (lower.includes('drive.google.com') || 
      lower.endsWith('.mp3') || 
      lower.endsWith('.wav') ||
      lower.endsWith('.webm') ||
      lower.endsWith('.ogg')) {
    return 'audio';
  }
  
  if (lower.startsWith('http://') || lower.startsWith('https://')) {
    return 'url';
  }
  
  return 'shortcut';
}
