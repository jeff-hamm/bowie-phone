/**
 * Bowie Phone Sequence Manager
 * Web UI for managing phone sequences backed by Google Sheets
 * Supports audio recording and upload to Google Drive
 */

class PhoneSequenceApp {
    constructor(config = {}) {
        this.config = config;
        
        // Google Sheets configuration
        this.sheetId = config.googleSheets?.spreadsheetId || null;
        this.gid = config.googleSheets?.gid || '0';
        this.appsScriptUrl = config.googleSheets?.appScriptUrl || null;
        this.useGoogleSheets = config.googleSheets?.enabled && this.sheetId;
        
        // Google Drive configuration for audio uploads
        this.driveFolderId = config.googleDrive?.folderId || null;
        
        // Data
        this.sequences = [];
        this.lastSync = null;
        this.isOnline = navigator.onLine;
        
        // Audio recording state
        this.mediaRecorder = null;
        this.audioChunks = [];
        this.recordedBlob = null;
        this.recordingStartTime = null;
        this.recordingTimer = null;
        
        // Edit state
        this.editingSequenceId = null;
        this.currentEditField = null;
    }

    init() {
        console.log('🚀 PhoneSequenceApp initializing...');
        
        // Set up event listeners
        this.setupEventListeners();
        
        // Set up audio recording
        this.setupAudioRecording();
        
        // Network status monitoring
        window.addEventListener('online', () => {
            this.isOnline = true;
            this.hideOfflineNotice();
            this.loadData();
        });
        
        window.addEventListener('offline', () => {
            this.isOnline = false;
            this.showOfflineNotice();
        });
        
        // Load initial data
        this.loadData();
        
        // Auto-refresh
        if (this.useGoogleSheets && this.config.dataSource?.syncInterval) {
            setInterval(() => this.loadData(), this.config.dataSource.syncInterval);
        }
        
        console.log('✅ PhoneSequenceApp initialized');
    }

    setupEventListeners() {
        // Add button
        const addBtn = document.getElementById('add-sequence-btn');
        if (addBtn) {
            addBtn.addEventListener('click', () => this.showAddModal());
        }
        
        // Form submission
        const form = document.getElementById('sequence-form');
        if (form) {
            form.addEventListener('submit', (e) => this.handleFormSubmit(e));
        }
        
        // Number validation
        const numberInput = document.getElementById('sequence-number');
        if (numberInput) {
            numberInput.addEventListener('input', (e) => this.validateNumber(e.target.value));
            numberInput.addEventListener('blur', (e) => this.validateNumber(e.target.value, true));
        }
        
        // Search
        const searchInput = document.getElementById('search-input');
        if (searchInput) {
            searchInput.addEventListener('input', (e) => this.filterSequences(e.target.value));
        }
        
        // URL toggle
        const urlToggle = document.getElementById('url-toggle');
        if (urlToggle) {
            urlToggle.addEventListener('click', () => this.toggleUrlInput());
        }
    }

    // ==================== DATA LOADING ====================
    
    async loadData() {
        console.log('🔄 Loading data...');
        
        if (!this.useGoogleSheets) {
            this.showError('Google Sheets must be configured. Please set up config.local.js');
            return;
        }
        
        if (!this.isOnline) {
            this.showOfflineNotice();
            return;
        }
        
        try {
            this.showLoading(true);
            await this.loadFromSheet();
            this.lastSync = new Date();
            this.updateSyncStatus('✅ Synced');
            this.renderSequences();
        } catch (error) {
            console.error('❌ Error loading data:', error);
            this.showError(`Failed to load: ${error.message}`);
        } finally {
            this.showLoading(false);
        }
    }

    async loadFromSheet() {
        // Try Apps Script first, fall back to CSV
        if (this.appsScriptUrl && 
            this.appsScriptUrl !== 'YOUR_GOOGLE_APPS_SCRIPT_URL_HERE' &&
            this.appsScriptUrl !== 'YOUR_UNIVERSAL_APPS_SCRIPT_URL_HERE') {
            return await this.loadFromAppsScript();
        } else {
            return await this.loadFromCSV();
        }
    }

    async loadFromAppsScript() {
        // Use JSONP to avoid CORS issues with Apps Script
        // Apps Script doesn't handle OPTIONS preflight from fetch POST
        const result = await this.makeJsonpRequest('getData', {
            sheetId: this.sheetId,
            gid: this.gid,
            driveFolderId: this.config.googleDrive?.folderId
        });
        
        if (!result.success) {
            throw new Error(result.error || 'Unknown error');
        }
        
        // Universal backend returns rows, map to sequences format
        this.sequences = result.data?.rows || result.data?.sequences || [];
        console.log(`✅ Loaded ${this.sequences.length} sequences from Apps Script`);
    }

    async loadFromCSV() {
        const csvUrl = `https://docs.google.com/spreadsheets/d/${this.sheetId}/gviz/tq?tqx=out:csv&gid=${this.gid}&headers=1`;
        
        const response = await fetch(csvUrl, {
            mode: 'cors',
            cache: 'no-cache'
        });
        
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}: Sheet may not be public`);
        }
        
        const csvText = await response.text();
        
        if (csvText.includes('<HTML>') || csvText.includes('<!DOCTYPE')) {
            throw new Error('Sheet is not publicly accessible');
        }
        
        this.parseCSV(csvText);
        console.log(`✅ Loaded ${this.sequences.length} sequences from CSV`);
    }

    parseCSV(csvText) {
        const lines = csvText.trim().split('\n');
        this.sequences = [];
        
        if (lines.length < 2) return;
        
        const headers = this.parseCSVLine(lines[0]).map(h => h.toLowerCase().trim());
        
        const nameIdx = this.findColumnIndex(headers, ['name', 'description', 'title']);
        const numberIdx = this.findColumnIndex(headers, ['number', 'sequence', 'dial']);
        const linkIdx = this.findColumnIndex(headers, ['link', 'url', 'audio', 'path']);
        
        for (let i = 1; i < lines.length; i++) {
            const cols = this.parseCSVLine(lines[i]);
            
            const name = nameIdx >= 0 ? cols[nameIdx]?.trim() : '';
            const number = numberIdx >= 0 ? cols[numberIdx]?.trim() : '';
            const link = linkIdx >= 0 ? cols[linkIdx]?.trim() : '';
            
            if (name || number) {
                this.sequences.push({
                    id: `row-${i + 1}`,
                    name: name,
                    number: number,
                    link: link,
                    rowIndex: i + 1
                });
            }
        }
    }

    parseCSVLine(line) {
        const result = [];
        let current = '';
        let inQuotes = false;
        
        for (let i = 0; i < line.length; i++) {
            const char = line[i];
            
            if (char === '"') {
                inQuotes = !inQuotes;
            } else if (char === ',' && !inQuotes) {
                result.push(current.replace(/^"|"$/g, ''));
                current = '';
            } else {
                current += char;
            }
        }
        
        result.push(current.replace(/^"|"$/g, ''));
        return result;
    }

    findColumnIndex(headers, possibleNames) {
        for (const name of possibleNames) {
            const idx = headers.indexOf(name);
            if (idx >= 0) return idx;
        }
        return -1;
    }

    // ==================== RENDERING ====================
    
    renderSequences(sequences = null) {
        const list = document.getElementById('sequence-list');
        if (!list) return;
        
        const toRender = sequences || this.sequences;
        
        if (toRender.length === 0) {
            list.innerHTML = `
                <div class="empty-state">
                    <div class="empty-state-icon">📞</div>
                    <div class="empty-state-text">No sequences yet</div>
                    <button class="btn btn-primary" onclick="phoneApp.showAddModal()">
                        ➕ Add Your First Sequence
                    </button>
                </div>
            `;
            return;
        }
        
        let html = '';
        
        // Sort by number
        const sorted = [...toRender].sort((a, b) => {
            return (a.number || '').localeCompare(b.number || '');
        });
        
        sorted.forEach(seq => {
            const linkDisplay = seq.link ? this.truncateUrl(seq.link, 30) : '<span class="empty-field">No audio/link</span>';
            const linkIcon = seq.link ? (seq.link.includes('drive.google.com') ? '🎵' : '🔗') : '';
            
            html += `
                <div class="sequence-item" data-id="${this.escapeHtml(seq.id)}">
                    <div class="sequence-item-content">
                        <div class="sequence-number editable-field" 
                             data-field="number" 
                             data-id="${this.escapeHtml(seq.id)}"
                             onclick="phoneApp.startEditing(this)">
                            ${this.escapeHtml(seq.number) || '<span class="empty-field">—</span>'}
                            <span class="edit-icon">✏️</span>
                        </div>
                        
                        <div class="sequence-name editable-field"
                             data-field="name"
                             data-id="${this.escapeHtml(seq.id)}"
                             onclick="phoneApp.startEditing(this)">
                            ${this.escapeHtml(seq.name) || '<span class="empty-field">Click to add name...</span>'}
                            <span class="edit-icon">✏️</span>
                        </div>
                        
                        <div class="sequence-link">
                            ${linkIcon}
                            <span class="sequence-link-text editable-field"
                                  data-field="link"
                                  data-id="${this.escapeHtml(seq.id)}"
                                  onclick="phoneApp.startEditingLink(this)">
                                ${linkDisplay}
                                <span class="edit-icon">✏️</span>
                            </span>
                            ${seq.link ? `<a href="${this.escapeHtml(seq.link)}" target="_blank" title="Open link" onclick="event.stopPropagation()">↗</a>` : ''}
                        </div>
                        
                        <button class="delete-btn" onclick="phoneApp.deleteSequence('${this.escapeHtml(seq.id)}')" title="Delete">
                            🗑️
                        </button>
                    </div>
                </div>
            `;
        });
        
        list.innerHTML = html;
    }

    truncateUrl(url, maxLen) {
        if (!url) return '';
        if (url.length <= maxLen) return this.escapeHtml(url);
        return this.escapeHtml(url.substring(0, maxLen) + '...');
    }

    filterSequences(query) {
        if (!query.trim()) {
            this.renderSequences();
            return;
        }
        
        const q = query.toLowerCase();
        const filtered = this.sequences.filter(seq => 
            (seq.name || '').toLowerCase().includes(q) ||
            (seq.number || '').toLowerCase().includes(q)
        );
        
        this.renderSequences(filtered);
    }

    // ==================== INLINE EDITING ====================
    
    startEditing(element) {
        const field = element.dataset.field;
        const id = element.dataset.id;
        const seq = this.sequences.find(s => s.id === id);
        
        if (!seq) return;
        
        // Don't start editing if already editing
        if (element.querySelector('input')) return;
        
        const currentValue = seq[field] || '';
        
        const input = document.createElement('input');
        input.type = 'text';
        input.className = 'editable-input' + (field === 'number' ? ' mono' : '');
        input.value = currentValue;
        
        input.addEventListener('blur', () => this.finishEditing(element, input, field, id));
        input.addEventListener('keydown', (e) => {
            if (e.key === 'Enter') {
                e.preventDefault();
                input.blur();
            } else if (e.key === 'Escape') {
                element.innerHTML = currentValue || `<span class="empty-field">—</span>`;
                element.innerHTML += '<span class="edit-icon">✏️</span>';
            }
        });
        
        if (field === 'number') {
            input.addEventListener('input', () => this.validateNumber(input.value));
        }
        
        element.innerHTML = '';
        element.appendChild(input);
        input.focus();
        input.select();
    }

    async finishEditing(element, input, field, id) {
        const newValue = input.value.trim();
        const seq = this.sequences.find(s => s.id === id);
        
        if (!seq) return;
        
        // Validate number field
        if (field === 'number' && newValue && !this.isValidNumber(newValue)) {
            element.innerHTML = seq[field] || `<span class="empty-field">—</span>`;
            element.innerHTML += '<span class="edit-icon">✏️</span>';
            return;
        }
        
        // Update local data
        seq[field] = newValue;
        
        // Re-render the field
        if (field === 'number') {
            element.innerHTML = newValue || `<span class="empty-field">—</span>`;
        } else {
            element.innerHTML = this.escapeHtml(newValue) || `<span class="empty-field">Click to add ${field}...</span>`;
        }
        element.innerHTML += '<span class="edit-icon">✏️</span>';
        
        // Sync to server
        await this.updateSequence(id, { [field]: newValue });
    }

    startEditingLink(element) {
        const id = element.dataset.id;
        this.editingSequenceId = id;
        
        const seq = this.sequences.find(s => s.id === id);
        if (!seq) return;
        
        // Open modal in edit mode for this field
        this.showEditModal(seq, 'link');
    }

    // ==================== MODAL HANDLING ====================
    
    showAddModal() {
        this.editingSequenceId = null;
        this.resetForm();
        
        document.getElementById('form-title').textContent = '➕ Add New Sequence';
        document.getElementById('submit-btn').textContent = 'Add Sequence';
        
        this.showModal();
    }

    showEditModal(sequence, focusField = null) {
        this.editingSequenceId = sequence.id;
        
        document.getElementById('form-title').textContent = '✏️ Edit Sequence';
        document.getElementById('submit-btn').textContent = 'Save Changes';
        
        // Fill form
        document.getElementById('sequence-name').value = sequence.name || '';
        document.getElementById('sequence-number').value = sequence.number || '';
        document.getElementById('sequence-link').value = sequence.link || '';
        document.getElementById('sequence-link-url').value = sequence.link || '';
        
        this.showModal();
        
        // Focus specific field if requested
        if (focusField === 'link') {
            this.toggleUrlInput();
            setTimeout(() => {
                document.getElementById('sequence-link-url').focus();
            }, 100);
        }
    }

    showModal() {
        document.getElementById('modal-backdrop').classList.add('show');
        document.getElementById('sequence-form').classList.add('show');
    }

    hideModal() {
        document.getElementById('modal-backdrop').classList.remove('show');
        document.getElementById('sequence-form').classList.remove('show');
        
        // Stop any recording
        this.stopRecording();
        this.resetAudioUI();
    }

    resetForm() {
        const form = document.getElementById('sequence-form');
        if (form) form.reset();
        
        document.getElementById('sequence-link').value = '';
        document.getElementById('number-validation').style.display = 'none';
        
        this.resetAudioUI();
        
        // Hide URL input
        document.getElementById('url-input-container').style.display = 'none';
        document.getElementById('url-toggle').style.display = 'block';
    }

    toggleUrlInput() {
        const container = document.getElementById('url-input-container');
        const toggle = document.getElementById('url-toggle');
        
        if (container.style.display === 'none') {
            container.style.display = 'block';
            toggle.textContent = 'Hide URL input';
        } else {
            container.style.display = 'none';
            toggle.textContent = 'Or enter a URL directly...';
        }
    }

    // ==================== NUMBER VALIDATION ====================
    
    validateNumber(value, showError = false) {
        const errorEl = document.getElementById('number-validation');
        const input = document.getElementById('sequence-number');
        
        if (!value.trim()) {
            errorEl.style.display = 'none';
            input.classList.remove('invalid');
            return true;
        }
        
        const isValid = this.isValidNumber(value);
        
        if (!isValid) {
            if (showError) {
                errorEl.textContent = 'Invalid: Use digits (0-9, *, #) or special names like "dialtone"';
                errorEl.style.display = 'block';
            }
            input.classList.add('invalid');
            return false;
        }
        
        errorEl.style.display = 'none';
        input.classList.remove('invalid');
        return true;
    }

    isValidNumber(value) {
        const trimmed = value.trim().toLowerCase();
        
        // Check if it's a special name
        const specialNumbers = this.config.specialNumbers || [
            'dialtone', 'busy', 'ringback', 'disconnect', 'error', 'silence'
        ];
        
        if (specialNumbers.includes(trimmed)) {
            return true;
        }
        
        // Check if it's a valid dial sequence (digits, *, #)
        const dialPattern = /^[0-9*#]+$/;
        return dialPattern.test(trimmed);
    }

    // ==================== AUDIO RECORDING ====================
    
    setupAudioRecording() {
        const recordBtn = document.getElementById('record-btn');
        const acceptBtn = document.getElementById('accept-audio-btn');
        const retryBtn = document.getElementById('retry-audio-btn');
        
        if (recordBtn) {
            recordBtn.addEventListener('click', () => this.toggleRecording());
        }
        
        if (acceptBtn) {
            acceptBtn.addEventListener('click', () => this.acceptRecording());
        }
        
        if (retryBtn) {
            retryBtn.addEventListener('click', () => this.retryRecording());
        }
    }

    async toggleRecording() {
        if (this.mediaRecorder && this.mediaRecorder.state === 'recording') {
            this.stopRecording();
        } else {
            await this.startRecording();
        }
    }

    async startRecording() {
        try {
            const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
            
            // Prefer MP3, then WAV, then WebM/Opus
            const candidates = [
                this.config.audio?.mimeType,
                'audio/mpeg',
                'audio/mp3',
                'audio/wav',
                'audio/webm;codecs=opus',
                'audio/webm'
            ].filter(Boolean);
            const mimeType = candidates.find(type => MediaRecorder.isTypeSupported(type)) || 'audio/webm';
            this.recordingMimeType = mimeType;
            
            this.mediaRecorder = new MediaRecorder(stream, { mimeType });
            this.audioChunks = [];
            
            this.mediaRecorder.ondataavailable = (e) => {
                if (e.data.size > 0) {
                    this.audioChunks.push(e.data);
                }
            };
            
            this.mediaRecorder.onstop = () => {
                this.recordedBlob = new Blob(this.audioChunks, { type: mimeType });
                this.showPlayback();
                
                // Stop all tracks
                stream.getTracks().forEach(track => track.stop());
            };
            
            this.mediaRecorder.start(100); // Collect data every 100ms
            this.recordingStartTime = Date.now();
            this.updateRecordingUI(true);
            this.startRecordingTimer();
            
            console.log('🎤 Recording started');
            
        } catch (error) {
            console.error('❌ Recording error:', error);
            this.showError('Microphone access denied. Please allow microphone access.');
        }
    }

    stopRecording() {
        if (this.mediaRecorder && this.mediaRecorder.state !== 'inactive') {
            this.mediaRecorder.stop();
            this.stopRecordingTimer();
            this.updateRecordingUI(false);
            console.log('🎤 Recording stopped');
        }
    }

    startRecordingTimer() {
        const timerEl = document.getElementById('recording-timer');
        timerEl.style.display = 'inline';
        
        this.recordingTimer = setInterval(() => {
            const elapsed = Math.floor((Date.now() - this.recordingStartTime) / 1000);
            const mins = Math.floor(elapsed / 60).toString().padStart(2, '0');
            const secs = (elapsed % 60).toString().padStart(2, '0');
            timerEl.textContent = `${mins}:${secs}`;
            
            // Check max duration
            const maxDuration = this.config.audio?.maxDuration || 300;
            if (elapsed >= maxDuration) {
                this.stopRecording();
            }
        }, 1000);
    }

    stopRecordingTimer() {
        if (this.recordingTimer) {
            clearInterval(this.recordingTimer);
            this.recordingTimer = null;
        }
    }

    updateRecordingUI(isRecording) {
        const recordBtn = document.getElementById('record-btn');
        
        if (isRecording) {
            recordBtn.classList.add('recording');
            recordBtn.title = 'Click to stop';
        } else {
            recordBtn.classList.remove('recording');
            recordBtn.title = 'Click to record';
        }
    }

    showPlayback() {
        const playback = document.getElementById('audio-playback');
        const player = document.getElementById('audio-player');
        const timerEl = document.getElementById('recording-timer');
        const recordBtn = document.getElementById('record-btn');
        
        // Hide record button, show playback
        recordBtn.style.display = 'none';
        timerEl.style.display = 'none';
        playback.style.display = 'flex';
        
        // Set audio source
        const audioUrl = URL.createObjectURL(this.recordedBlob);
        player.src = audioUrl;
    }

    async acceptRecording() {
        if (!this.recordedBlob) return;
        
        // Show upload status
        const statusEl = document.getElementById('upload-status');
        statusEl.style.display = 'flex';
        statusEl.className = 'upload-status uploading';
        statusEl.querySelector('.status-text').textContent = 'Uploading to Google Drive...';
        
        try {
            const url = await this.uploadAudioToDrive(this.recordedBlob);
            
            if (url) {
                // Set the link value
                document.getElementById('sequence-link').value = url;
                document.getElementById('sequence-link-url').value = url;
                
                statusEl.className = 'upload-status success';
                statusEl.querySelector('.status-text').textContent = '✓ Uploaded successfully';
                
                console.log('✅ Audio uploaded:', url);
            } else {
                throw new Error('No URL returned');
            }
            
        } catch (error) {
            console.error('❌ Upload error:', error);
            statusEl.className = 'upload-status error';
            statusEl.querySelector('.status-text').textContent = `Upload failed: ${error.message}`;
        }
    }

    retryRecording() {
        this.recordedBlob = null;
        this.audioChunks = [];
        this.resetAudioUI();
    }

    resetAudioUI() {
        const playback = document.getElementById('audio-playback');
        const recordBtn = document.getElementById('record-btn');
        const timerEl = document.getElementById('recording-timer');
        const statusEl = document.getElementById('upload-status');
        
        if (playback) playback.style.display = 'none';
        if (recordBtn) {
            recordBtn.style.display = 'flex';
            recordBtn.classList.remove('recording');
        }
        if (timerEl) {
            timerEl.style.display = 'none';
            timerEl.textContent = '00:00';
        }
        if (statusEl) statusEl.style.display = 'none';
        
        this.recordedBlob = null;
        this.audioChunks = [];
    }

    async uploadAudioToDrive(blob) {
        if (!this.appsScriptUrl || 
            this.appsScriptUrl === 'YOUR_GOOGLE_APPS_SCRIPT_URL_HERE' ||
            this.appsScriptUrl === 'YOUR_UNIVERSAL_APPS_SCRIPT_URL_HERE') {
            throw new Error('Apps Script URL not configured');
        }
        
        // Convert blob to base64
        const base64 = await this.blobToBase64(blob);
        
        // Generate filename with extension based on mime type (prefer mp3, else wav, else webm)
        const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
        const prefix = this.config.audio?.filePrefix || 'bowie-phone-recording';
        const mime = this.recordingMimeType || blob.type || 'audio/webm';
        let extension = '.webm';
        if (mime.includes('mpeg') || mime.includes('mp3')) extension = '.mp3';
        else if (mime.includes('wav')) extension = '.wav';
        const fileName = `${prefix}_${timestamp}${extension}`;
        
        // Send to Apps Script (includes config for universal backend)
        // Use text/plain to keep the request a simple POST (avoids CORS preflight)
        const response = await fetch(this.appsScriptUrl, {
            method: 'POST',
            headers: {
                'Content-Type': 'text/plain;charset=utf-8'
            },
            body: JSON.stringify({
                action: 'uploadFile',
                config: {
                    sheetId: this.sheetId,
                    gid: this.gid,
                    driveFolderId: this.config.googleDrive?.folderId
                },
                fileName: fileName,
                fileData: base64,
                mimeType: mime,
                driveFolder: this.driveFolderId
            })
        });
        
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }
        
        const result = await response.json();
        
        if (!result.success) {
            throw new Error(result.error || 'Upload failed');
        }
        
        return result.data?.fileUrl || result.fileUrl;
    }

    blobToBase64(blob) {
        return new Promise((resolve, reject) => {
            const reader = new FileReader();
            reader.onload = () => {
                const base64 = reader.result.split(',')[1];
                resolve(base64);
            };
            reader.onerror = reject;
            reader.readAsDataURL(blob);
        });
    }

    // ==================== FORM SUBMISSION ====================
    
    async handleFormSubmit(e) {
        e.preventDefault();
        
        const name = document.getElementById('sequence-name').value.trim();
        const number = document.getElementById('sequence-number').value.trim();
        
        // Get link from hidden input (set by recording) or URL input
        let link = document.getElementById('sequence-link').value.trim();
        if (!link) {
            link = document.getElementById('sequence-link-url').value.trim();
        }
        
        // Validate number
        if (!this.validateNumber(number, true)) {
            return;
        }
        
        const data = { name, number, link };
        
        // Show loading
        document.getElementById('form-loading-overlay').style.display = 'flex';
        
        try {
            if (this.editingSequenceId) {
                await this.updateSequence(this.editingSequenceId, data);
            } else {
                await this.addSequence(data);
            }
            
            this.hideModal();
            await this.loadData();
            
        } catch (error) {
            console.error('❌ Form submit error:', error);
            this.showError(`Failed to save: ${error.message}`);
        } finally {
            document.getElementById('form-loading-overlay').style.display = 'none';
        }
    }

    // ==================== CRUD OPERATIONS ====================
    
    async addSequence(data) {
        if (!this.appsScriptUrl || 
            this.appsScriptUrl === 'YOUR_GOOGLE_APPS_SCRIPT_URL_HERE' ||
            this.appsScriptUrl === 'YOUR_UNIVERSAL_APPS_SCRIPT_URL_HERE') {
            // Local only mode
            const newSeq = {
                id: `local-${Date.now()}`,
                ...data
            };
            this.sequences.push(newSeq);
            this.renderSequences();
            this.updateSyncStatus('⚠️ Added locally only');
            return;
        }
        
        // Use JSONP to avoid CORS issues
        const result = await this.makeJsonpRequest('addRow', {
            sheetId: this.sheetId,
            gid: this.gid,
            driveFolderId: this.config.googleDrive?.folderId,
            rowData: JSON.stringify(data)
        });
        
        if (!result.success) {
            throw new Error(result.error || 'Add failed');
        }
        
        this.updateSyncStatus('✅ Added');
    }

    async updateSequence(id, updates) {
        // Update local data
        const seq = this.sequences.find(s => s.id === id);
        if (seq) {
            Object.assign(seq, updates);
        }
        
        if (!this.appsScriptUrl || 
            this.appsScriptUrl === 'YOUR_GOOGLE_APPS_SCRIPT_URL_HERE' ||
            this.appsScriptUrl === 'YOUR_UNIVERSAL_APPS_SCRIPT_URL_HERE') {
            this.updateSyncStatus('⚠️ Updated locally only');
            return;
        }
        
        try {
            // Use JSONP to avoid CORS issues
            const result = await this.makeJsonpRequest('updateRow', {
                sheetId: this.sheetId,
                gid: this.gid,
                driveFolderId: this.config.googleDrive?.folderId,
                rowId: id,
                updates: JSON.stringify(updates)
            });
            
            if (!result.success) {
                throw new Error(result.error || 'Update failed');
            }
            
            this.updateSyncStatus('✅ Updated');
            
        } catch (error) {
            console.error('❌ Update error:', error);
            this.updateSyncStatus('⚠️ Update failed');
        }
    }

    async deleteSequence(id) {
        if (!confirm('Delete this sequence?')) return;
        
        // Remove from local data
        this.sequences = this.sequences.filter(s => s.id !== id);
        this.renderSequences();
        
        if (!this.appsScriptUrl || 
            this.appsScriptUrl === 'YOUR_GOOGLE_APPS_SCRIPT_URL_HERE' ||
            this.appsScriptUrl === 'YOUR_UNIVERSAL_APPS_SCRIPT_URL_HERE') {
            this.updateSyncStatus('⚠️ Deleted locally only');
            return;
        }
        
        try {
            // Use JSONP to avoid CORS issues
            const result = await this.makeJsonpRequest('deleteRow', {
                sheetId: this.sheetId,
                gid: this.gid,
                rowId: id
            });
            
            if (!result.success) {
                throw new Error(result.error || 'Delete failed');
            }
            
            this.updateSyncStatus('✅ Deleted');
            
        } catch (error) {
            console.error('❌ Delete error:', error);
            this.updateSyncStatus('⚠️ Delete failed');
        }
    }

    // ==================== UI HELPERS ====================
    
    showLoading(show) {
        const list = document.getElementById('sequence-list');
        if (show && list) {
            list.innerHTML = '<div class="loading">Loading sequences...</div>';
        }
    }

    showError(message) {
        const errorEl = document.getElementById('error-display');
        if (errorEl) {
            errorEl.textContent = message;
            errorEl.style.display = 'block';
            
            setTimeout(() => {
                errorEl.style.display = 'none';
            }, 5000);
        }
    }

    showOfflineNotice() {
        const notice = document.getElementById('offline-notice');
        if (notice) notice.style.display = 'block';
    }

    hideOfflineNotice() {
        const notice = document.getElementById('offline-notice');
        if (notice) notice.style.display = 'none';
    }

    updateSyncStatus(text) {
        const statusEl = document.getElementById('sync-status-text');
        if (statusEl) statusEl.textContent = text;
    }

    escapeHtml(str) {
        if (!str) return '';
        const div = document.createElement('div');
        div.textContent = str;
        return div.innerHTML;
    }

    // ==================== JSONP HELPER ====================
    
    /**
     * Make a JSONP request to Apps Script
     * This bypasses CORS by using a script tag instead of fetch
     * Apps Script can't handle OPTIONS preflight requests, so we use GET with JSONP
     */
    makeJsonpRequest(action, params = {}) {
        return new Promise((resolve, reject) => {
            const callbackName = `jsonp_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
            const timeout = setTimeout(() => {
                cleanup();
                reject(new Error('JSONP request timeout'));
            }, 30000);

            const cleanup = () => {
                clearTimeout(timeout);
                delete window[callbackName];
                const script = document.querySelector(`script[data-jsonp="${callbackName}"]`);
                if (script) script.remove();
            };

            window[callbackName] = (response) => {
                cleanup();
                resolve(response);
            };

            // Build URL with all params
            const url = new URL(this.appsScriptUrl);
            url.searchParams.set('callback', callbackName);
            url.searchParams.set('action', action);
            
            // Add all params to URL
            for (const [key, value] of Object.entries(params)) {
                if (value !== undefined && value !== null) {
                    url.searchParams.set(key, value);
                }
            }

            const script = document.createElement('script');
            script.src = url.toString();
            script.dataset.jsonp = callbackName;
            script.onerror = () => {
                cleanup();
                reject(new Error('JSONP script load failed'));
            };

            document.body.appendChild(script);
        });
    }
}

// Export for use in HTML
window.PhoneSequenceApp = PhoneSequenceApp;
