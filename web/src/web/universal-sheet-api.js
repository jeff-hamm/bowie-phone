/**
 * Universal Sheet API Client v2.0
 * 
 * A fully dynamic client that works with any Google Sheet structure.
 * Headers become property names automatically via camelCase conversion.
 * 
 * Usage:
 *   const api = new UniversalSheetAPI(APPS_SCRIPT_URL, {
 *     sheetId: 'your-sheet-id',
 *     gid: '0',
 *     driveFolderId: 'your-drive-folder-id',
 *     schema: {  // Optional: provide type hints
 *       'done?': { type: 'boolean', key: 'done' },
 *       'due date': { type: 'date', key: 'dueDate' }
 *     }
 *   });
 *   
 *   const { rows, schema } = await api.getData();
 *   await api.addRow({ name: 'Test', number: '123' });
 *   await api.updateRow('row-5', { name: 'Updated' });
 * 
 * Header Conversion Examples:
 *   "Name" -> "name"
 *   "Due Date" -> "dueDate"
 *   "Done?" -> "done"
 *   "Who Can Help" -> "whoCanHelp"
 *   "Pumpkin Patch" -> "pumpkinPatch"
 */

class UniversalSheetAPI {
    /**
     * Create a new API client
     * @param {string} appsScriptUrl - The deployed Apps Script URL
     * @param {Object} config - Configuration options
     * @param {string} config.sheetId - Google Sheet ID (required)
     * @param {string} [config.gid='0'] - Sheet GID (tab)
     * @param {string} [config.schemaGid] - Optional GID of schema definition sheet
     * @param {string} [config.driveFolderId] - Google Drive folder for uploads
     * @param {Object} [config.schema] - Optional schema for type coercion
     */
    constructor(appsScriptUrl, config = {}) {
        this.url = appsScriptUrl;
        this.config = {
            sheetId: config.sheetId,
            gid: config.gid || '0',
            schemaGid: config.schemaGid || null,
            driveFolderId: config.driveFolderId || null
        };
        this.schema = config.schema || null;
        this._cachedSchema = null;
    }

    /**
     * Make a request to the Apps Script backend
     */
    async request(action, data = {}) {
        const payload = {
            action,
            config: this.config,
            ...data
        };

        // Include schema if provided
        if (this.schema) {
            payload.schema = this.schema;
        }

        try {
            const response = await fetch(this.url, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(payload)
            });

            if (!response.ok) {
                throw new Error(`HTTP ${response.status}: ${response.statusText}`);
            }

            const result = await response.json();

            if (!result.success) {
                throw new Error(result.error || 'Request failed');
            }

            return result.data;
        } catch (error) {
            console.error(`API request failed (${action}):`, error);
            throw error;
        }
    }

    // ==================== CORE CRUD ====================

    /**
     * Get all rows from the sheet
     * Returns { rows, headers, schema, lastModified }
     * 
     * Row properties are camelCase versions of headers:
     *   "Done?" -> row.done
     *   "Due Date" -> row.dueDate
     */
    async getData() {
        const result = await this.request('getData');
        this._cachedSchema = result.schema;
        return result;
    }

    /**
     * Alias for getData()
     */
    async getRows() {
        return this.getData();
    }

    /**
     * Add a new row
     * @param {Object} rowData - Data for the new row
     *   Can use either header names or schema keys:
     *   { "Done?": true, "Task": "..." } or { done: true, task: "..." }
     */
    async addRow(rowData) {
        return this.request('addRow', { rowData });
    }

    /**
     * Update an existing row
     * @param {string} rowId - Row ID (e.g., 'row-5')
     * @param {Object} updates - Fields to update
     */
    async updateRow(rowId, updates) {
        return this.request('updateRow', { rowId, updates });
    }

    /**
     * Delete a row
     * @param {string} rowId - Row ID to delete
     */
    async deleteRow(rowId) {
        return this.request('deleteRow', { rowId });
    }

    // ==================== SCHEMA ====================

    /**
     * Get schema information for the sheet
     */
    async getSchema() {
        return this.request('getSchema');
    }

    /**
     * Set schema for type coercion
     * @param {Object} schema - Schema definition
     */
    setSchema(schema) {
        this.schema = schema;
    }

    // ==================== FILE OPERATIONS ====================

    /**
     * Upload a file to Google Drive
     */
    async uploadFile(fileName, base64Data, mimeType) {
        return this.request('uploadFile', {
            fileName,
            fileData: base64Data,
            mimeType
        });
    }

    /**
     * Upload a Blob directly
     */
    async uploadBlob(fileName, blob) {
        const base64 = await this.blobToBase64(blob);
        return this.uploadFile(fileName, base64, blob.type);
    }

    /**
     * Get files from the configured Drive folder
     */
    async getFiles() {
        return this.request('getFiles');
    }

    // ==================== UTILITY ====================

    /**
     * Get sheet metadata
     */
    async getSheetInfo() {
        return this.request('getSheetInfo');
    }

    /**
     * Get last modified timestamp
     */
    async getLastModified() {
        return this.request('getLastModified');
    }

    /**
     * Export data as JSON keyed by a field
     * @param {string} keyField - Field to use as key (e.g., 'number' for bowie-phone)
     */
    async exportAsJSON(keyField) {
        return this.request('exportAsJSON', { keyField });
    }

    /**
     * Test the connection
     */
    async test() {
        const response = await fetch(`${this.url}?action=test`);
        return response.json();
    }

    /**
     * Convert a Blob to base64
     */
    blobToBase64(blob) {
        return new Promise((resolve, reject) => {
            const reader = new FileReader();
            reader.onloadend = () => resolve(reader.result.split(',')[1]);
            reader.onerror = reject;
            reader.readAsDataURL(blob);
        });
    }

    // ==================== LEGACY ALIASES ====================
    // These provide backward compatibility with app-specific APIs

    // To-do aliases
    async getTasks() { return this.getData(); }
    async addTask(task) { return this.addRow(task); }
    async updateTask(taskId, completed) { 
        return this.updateRow(taskId, { 'done?': completed }); 
    }
    async updateTaskDetails(taskId, updates) { 
        return this.updateRow(taskId, updates); 
    }
    async deleteTask(taskId) { return this.deleteRow(taskId); }

    // Bowie Phone aliases
    async getSequences() { return this.getData(); }
    async addSequence(seq) { return this.addRow(seq); }
    async updateSequence(seqId, updates) { 
        return this.updateRow(seqId, updates); 
    }
    async deleteSequence(seqId) { return this.deleteRow(seqId); }
    async exportSequencesAsJSON() { return this.exportAsJSON('number'); }

    // Pumpkinfest aliases
    async getRSVPs() { return this.getData(); }
    async addRSVP(rsvp) { return this.addRow(rsvp); }
    async updateRSVP(rsvp) { 
        return this.request('updateRSVP', { rsvp }); 
    }

    // File aliases
    async getGalleryImages() { return this.getFiles(); }
}

// ==================== SCHEMA HELPERS ====================

/**
 * Create a schema definition for common patterns
 */
UniversalSheetAPI.createSchema = {
    /**
     * To-do schema
     * Headers: Done? | Task | Category | How | Timeline | Priority | Who Can Help | Notes | Due Date
     */
    todo: {
        'done?': { type: 'boolean', key: 'done' },
        'task': { type: 'string', key: 'task' },
        'category': { type: 'string', key: 'category' },
        'how': { type: 'string', key: 'how' },
        'timeline': { type: 'string', key: 'timeline' },
        'priority': { type: 'string', key: 'priority' },
        'who can help': { type: 'string', key: 'whoCanHelp' },
        'notes': { type: 'string', key: 'notes' },
        'due date': { type: 'date', key: 'dueDate' }
    },

    /**
     * Bowie Phone schema
     * Headers: Name | Number | Link
     */
    bowiePhone: {
        'name': { type: 'string', key: 'name' },
        'number': { type: 'string', key: 'number' },
        'link': { type: 'string', key: 'link' }
    },

    /**
     * Pumpkinfest schema
     * Headers: Name | Attendance | Need Pumpkin | Bringing | Pumpkin Patch | Patch Dates | Timestamp | Email
     */
    pumpkinfest: {
        'name': { type: 'string', key: 'name' },
        'attendance': { type: 'string', key: 'attendance' },
        'need pumpkin': { type: 'string', key: 'needPumpkin' },
        'bringing': { type: 'string', key: 'bringing' },
        'pumpkin patch': { type: 'string', key: 'pumpkinPatch' },
        'patch dates': { type: 'string', key: 'patchDates' },
        'timestamp': { type: 'date', key: 'timestamp' },
        'email': { type: 'string', key: 'email' }
    }
};

/**
 * Convert a header string to camelCase key
 * Matches the server-side toKey() function
 */
UniversalSheetAPI.toKey = function(header) {
    return header
        .toLowerCase()
        .replace(/[?!]/g, '')
        .trim()
        .split(/[\s_-]+/)
        .map((word, i) => i === 0 ? word : word.charAt(0).toUpperCase() + word.slice(1))
        .join('');
};

// Export for module systems
if (typeof module !== 'undefined' && module.exports) {
    module.exports = UniversalSheetAPI;
}
