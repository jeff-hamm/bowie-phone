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
        this.listAudioPlayer = null;
        
        // Edit state
        this.editingSequenceId = null;
        this.currentEditField = null;

        // List playback state
        this.listAudioPlayer = null;
        this.listPlayingId = null;
        this.listPlaybackButton = null;
        this.listPlaybackTimer = null;
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
            numberInput.addEventListener('input', () => this.updateSubmitButtonState());
            numberInput.addEventListener('blur', () => this.updateSubmitButtonState());
        }
        const nameInput = document.getElementById('sequence-name');
        if (nameInput) {
            nameInput.addEventListener('input', () => this.updateSubmitButtonState());
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

        const linkUrlInput = document.getElementById('sequence-link-url');
        if (linkUrlInput) {
            linkUrlInput.addEventListener('input', () => this.updateSubmitButtonState());
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
            this.updateSyncStatus('⚠️ Refresh failed');
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
                    <button class="btn btn-primary add-first-btn" onclick="phoneApp.showAddModal()">
                        Add Your First Sequence
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
            const playableLink = seq.link ? this.getPlayableUrl(seq.link) : '';
            const linkHref = seq.link ? this.escapeHtml(seq.link) : '';
            
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
                            <div class="sequence-play-wrap">
                                <button class="sequence-play-btn" ${seq.link ? `onclick="phoneApp.playSequenceLink('${this.escapeHtml(seq.id)}', event)"` : 'disabled title="Add an audio/link to enable playback"'} aria-label="Play audio">
                                    ▶ Play
                                </button>
                                <span class="play-timer" style="display:none;">00:00 / --:--</span>
                            </div>
                            <span class="sequence-link-text editable-field"
                                  data-field="link"
                                  data-id="${this.escapeHtml(seq.id)}"
                                  onclick="phoneApp.startEditingLink(this)">
                                ${linkDisplay}
                                <span class="edit-icon">✏️</span>
                            </span>
                            ${seq.link ? `<a href="${linkHref}" target="_blank" title="Open link" onclick="event.stopPropagation()">↗</a>` : ''}
                        </div>

                        <div class="sequence-actions">
                            <button class="icon-btn edit-btn" onclick="phoneApp.showEditModalById('${this.escapeHtml(seq.id)}')" title="Open edit dialog">✏️</button>
                            <button class="icon-btn delete-btn" onclick="phoneApp.deleteSequence('${this.escapeHtml(seq.id)}')" title="Delete">🗑️</button>
                        </div>
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
        
        const formTitle = document.getElementById('form-title');
        formTitle.textContent = 'Add New Sequence';
        formTitle.className = 'form-title-add';
        document.getElementById('submit-btn').textContent = 'Add Sequence';

        this.updateSubmitButtonState();
        
        this.showModal();
    }

    showEditModalById(id) {
        const seq = this.sequences.find(s => s.id === id);
        if (!seq) {
            console.warn('Sequence not found for edit dialog:', id);
            return;
        }
        this.showEditModal(seq);
    }

    showEditModal(sequence, focusField = null) {
        this.editingSequenceId = sequence.id;
        this.resetAudioUI();
        const urlContainer = document.getElementById('url-input-container');
        const urlToggle = document.getElementById('url-toggle');
        if (urlContainer && urlToggle) {
            urlContainer.style.display = 'none';
            urlToggle.textContent = 'Show URL input';
        }
        
        const formTitle = document.getElementById('form-title');
        formTitle.textContent = 'Edit Sequence';
        formTitle.className = 'form-title-edit';
        document.getElementById('submit-btn').textContent = 'Save Changes';
        
        // Fill form
        document.getElementById('sequence-name').value = sequence.name || '';
        document.getElementById('sequence-number').value = sequence.number || '';
        document.getElementById('sequence-link').value = sequence.link || '';
        document.getElementById('sequence-link-url').value = sequence.link || '';
        
        this.showModal();

        this.updateSubmitButtonState();
        
        // If an audio/link already exists, show playback by default and hide record until re-record is chosen
        if (sequence.link) {
            this.showPlayback(sequence.link, { showAccept: false });
        }
        
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

        this.updateSubmitButtonState();
        
        // Hide URL input
        document.getElementById('url-input-container').style.display = 'none';
        document.getElementById('url-toggle').style.display = 'block';
        document.getElementById('url-toggle').textContent = 'Show URL input';
    }

    toggleUrlInput() {
        const container = document.getElementById('url-input-container');
        const toggle = document.getElementById('url-toggle');
        
        if (container.style.display === 'none') {
            container.style.display = 'block';
            toggle.textContent = 'Hide URL input';
        } else {
            container.style.display = 'none';
            toggle.textContent = 'Show URL input';
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

    isValidUrl(value) {
        if (!value) return false;
        try {
            const parsed = new URL(value);
            return parsed.protocol === 'http:' || parsed.protocol === 'https:';
        } catch {
            return false;
        }
    }

    updateSubmitButtonState() {
        const submitBtn = document.getElementById('submit-btn');
        const linkInput = document.getElementById('sequence-link-url');
        const hiddenLinkInput = document.getElementById('sequence-link');
        const nameInput = document.getElementById('sequence-name');
        const numberInput = document.getElementById('sequence-number');
        if (!submitBtn) return;

        const linkValue = (hiddenLinkInput?.value || '').trim() || (linkInput?.value || '').trim();
        const nameValue = (nameInput?.value || '').trim();
        const numberValue = (numberInput?.value || '').trim();

        const nameOk = !!nameValue;
        const numberOk = !!numberValue && this.validateNumber(numberValue, false);
        const linkOk = this.isValidUrl(linkValue);

        const valid = nameOk && numberOk && linkOk;

        submitBtn.disabled = !valid;
        if (!nameOk) {
            submitBtn.title = 'Enter a name to enable saving';
        } else if (!numberOk) {
            submitBtn.title = 'Enter a valid number to enable saving';
        } else if (!linkOk) {
            submitBtn.title = 'Add a valid audio/link URL to enable saving';
        } else {
            submitBtn.title = '';
        }
    }

    // ==================== AUDIO RECORDING ====================
    
    setupAudioRecording() {
        const recordBtn = document.getElementById('record-btn');
        const acceptBtn = document.getElementById('accept-audio-btn');
        const retryBtn = document.getElementById('retry-audio-btn');
        const uploadBtn = document.getElementById('upload-file-btn');
        const fileInput = document.getElementById('audio-file-input');
        
        if (recordBtn) {
            recordBtn.addEventListener('click', () => this.toggleRecording());
        }
        
        if (acceptBtn) {
            acceptBtn.addEventListener('click', () => this.acceptRecording());
        }
        
        if (retryBtn) {
            retryBtn.addEventListener('click', () => this.retryRecording());
        }
        
        if (uploadBtn) {
            uploadBtn.addEventListener('click', () => {
                fileInput?.click();
            });
        }
        
        if (fileInput) {
            fileInput.addEventListener('change', (e) => this.handleFileSelect(e));
        }
    }

    async handleFileSelect(event) {
        const file = event.target.files?.[0];
        if (!file) return;
        
        // Validate file type
        const validTypes = ['audio/mpeg', 'audio/mp3', 'audio/mp4', 'audio/m4a', 'audio/wav', 'audio/ogg', 'audio/aac', 'audio/x-m4a'];
        const validExtensions = ['.mp3', '.m4a', '.wav', '.ogg', '.aac'];
        const hasValidType = validTypes.some(type => file.type.includes(type.split('/')[1]));
        const hasValidExt = validExtensions.some(ext => file.name.toLowerCase().endsWith(ext));
        
        if (!hasValidType && !hasValidExt) {
            this.showError('Invalid file type. Please select an audio file (MP3, M4A, WAV, OGG, or AAC).');
            event.target.value = '';
            return;
        }
        
        // Check file size (50MB limit)
        const maxSize = 50 * 1024 * 1024;
        if (file.size > maxSize) {
            this.showError('File is too large. Maximum size is 50MB.');
            event.target.value = '';
            return;
        }
        
        // Store the file as a blob
        this.recordedBlob = file;
        this.recordingMimeType = file.type || 'audio/wav';
        
        // Update UI
        const fileNameDisplay = document.getElementById('file-name-display');
        if (fileNameDisplay) {
            fileNameDisplay.textContent = `✓ ${file.name}`;
            fileNameDisplay.style.color = 'var(--success-color, #4CAF50)';
        }
        
        console.log(`📁 File selected: ${file.name} (${(file.size / 1024 / 1024).toFixed(2)} MB)`);
        
        // Show playback preview
        const fileUrl = URL.createObjectURL(file);
        await this.showPlayback(fileUrl, { showAccept: true });
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
            
            // Prefer AAC/MP4 (M4A), then MP3, then WAV; allow WebM as capture fallback (converted before upload)
            const candidates = [
                this.config.audio?.mimeType,
                'audio/mp4;codecs=aac',
                'audio/mp4;codecs=mp4a.40.2',
                'audio/mp4',
                'audio/m4a',
                'audio/aac',
                'audio/mpeg',
                'audio/mp3',
                'audio/wav',
                'audio/webm;codecs=opus',
                'audio/webm'
            ].filter(Boolean);
            const mimeType = candidates.find(type => MediaRecorder.isTypeSupported(type));
            if (!mimeType) {
                throw new Error('Browser does not support M4A/MP3/WAV/WebM recording.');
            }
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

    formatTime(seconds) {
        if (!isFinite(seconds) || seconds < 0) return '--:--';
        const mins = Math.floor(seconds / 60).toString().padStart(2, '0');
        const secs = Math.floor(seconds % 60).toString().padStart(2, '0');
        return `${mins}:${secs}`;
    }

    resetListPlaybackUI() {
        if (this.listAudioPlayer) {
            try { this.listAudioPlayer.pause(); } catch {}
        }
        this.listAudioPlayer = null;
        this.listPlayingId = null;

        if (this.listPlaybackButton) {
            this.listPlaybackButton.classList.remove('is-loading', 'is-playing');
            this.listPlaybackButton.disabled = false;
            this.listPlaybackButton.innerHTML = '▶ Play';
        }

        if (this.listPlaybackTimer) {
            this.listPlaybackTimer.textContent = '00:00 / --:--';
            this.listPlaybackTimer.style.display = 'none';
        }

        this.listPlaybackButton = null;
        this.listPlaybackTimer = null;
    }

    async playSequenceLink(sequenceId, event = null) {
        if (event) {
            event.stopPropagation();
        }

        const sequence = this.sequences.find(seq => seq.id === sequenceId);
        if (!sequence || !sequence.link) {
            this.showError('No audio/link to play yet.');
            return;
        }

        // If clicking the currently playing sequence, stop it
        if (this.listPlayingId === sequenceId && this.listAudioPlayer && !this.listAudioPlayer.paused) {
            this.resetListPlaybackUI();
            return;
        }

        // Reset any existing playback UI
        this.resetListPlaybackUI();

        const button = event?.currentTarget;
        const timerEl = button?.closest('.sequence-item-content')?.querySelector('.play-timer');
        if (button) {
            button.disabled = true;
            button.classList.add('is-loading');
            button.innerHTML = '⏳';
        }
        if (timerEl) {
            timerEl.textContent = '00:00 / --:--';
            timerEl.style.display = 'none';
        }

        try {
            const targetUrl = this.getPlayableUrl(sequence.link) || sequence.link;
            if (!targetUrl) {
                throw new Error('Invalid audio URL');
            }

            let playbackUrl = targetUrl;
            if (targetUrl.includes('action=getDriveFile')) {
                playbackUrl = await this.fetchProxyAudioAsBlobUrl(targetUrl) || targetUrl;
            } else {
                playbackUrl = await this.fetchAsBlobUrl(targetUrl) || targetUrl;
            }

            this.listAudioPlayer = new Audio(playbackUrl);
            this.listPlayingId = sequenceId;
            this.listPlaybackButton = button || null;
            this.listPlaybackTimer = timerEl || null;

            this.listAudioPlayer.addEventListener('loadedmetadata', () => {
                const total = this.formatTime(this.listAudioPlayer.duration);
                if (this.listPlaybackTimer) {
                    this.listPlaybackTimer.textContent = `00:00 / ${total}`;
                    this.listPlaybackTimer.style.display = 'block';
                }
            });

            this.listAudioPlayer.addEventListener('timeupdate', () => {
                if (this.listPlaybackTimer) {
                    const current = this.formatTime(this.listAudioPlayer.currentTime);
                    const total = this.formatTime(this.listAudioPlayer.duration);
                    this.listPlaybackTimer.textContent = `${current} / ${total}`;
                }
            });

            this.listAudioPlayer.addEventListener('ended', () => {
                this.resetListPlaybackUI();
            });

            await this.listAudioPlayer.play();
            this.updateSyncStatus('▶️ Playing audio');

            if (button) {
                button.disabled = false;
                button.classList.remove('is-loading');
                button.classList.add('is-playing');
                button.innerHTML = '⏹ Stop';
            }
            if (timerEl) {
                timerEl.style.display = 'block';
            }
        } catch (error) {
            console.warn('Playback failed:', error);
            this.showError('Could not play audio. Check the link or proxy access.');
            this.resetListPlaybackUI();
        }
    }

    async showPlayback(sourceUrl = null, { showAccept = true } = {}) {
        const playback = document.getElementById('audio-playback');
        const player = document.getElementById('audio-player');
        const timerEl = document.getElementById('recording-timer');
        const recordBtn = document.getElementById('record-btn');
        const acceptBtn = document.getElementById('accept-audio-btn');
        const statusEl = document.getElementById('upload-status');
        const setPlaybackLoading = (state) => {
            if (playback) {
                playback.classList[state ? 'add' : 'remove']('loading');
            }
        };
        
        // Hide record/timer; show playback
        if (recordBtn) recordBtn.style.display = 'none';
        if (timerEl) timerEl.style.display = 'none';
        if (statusEl) statusEl.style.display = 'none';
        if (playback) playback.style.display = 'flex';
        if (acceptBtn) acceptBtn.style.display = showAccept ? '' : 'none';
        
        // Set audio source (prefer provided URL, else recorded blob)
        const audioUrl = sourceUrl || (this.recordedBlob ? URL.createObjectURL(this.recordedBlob) : null);
        const playableUrl = this.getPlayableUrl(audioUrl);
        const finalUrl = playableUrl || audioUrl;

        if (player && finalUrl) {
            setPlaybackLoading(true);
            player.addEventListener('loadeddata', () => setPlaybackLoading(false), { once: true });
            // For proxied Drive audio, fetch base64 and convert to blob URL
            const isProxyDrive = finalUrl.includes('action=getDriveFile');
            if (isProxyDrive) {
                const blobUrl = await this.fetchProxyAudioAsBlobUrl(finalUrl);
                player.src = blobUrl || finalUrl;
            } else {
                const blobUrl = await this.fetchAsBlobUrl(finalUrl);
                player.src = blobUrl || finalUrl;
            }
            setPlaybackLoading(false);
        }
    }

    /**
     * Fetch audio from Apps Script proxy (returns base64) and convert to blob URL
     */
    async fetchProxyAudioAsBlobUrl(proxyUrl) {
        try {
            const response = await fetch(proxyUrl);
            if (!response.ok) {
                console.warn('Proxy audio fetch failed:', response.status);
                return null;
            }
            const json = await response.json();
            if (!json.success || !json.data?.base64) {
                console.warn('Proxy audio response invalid:', json.error || 'no base64 data');
                return null;
            }
            
            // Decode base64 to binary
            const binaryString = atob(json.data.base64);
            const bytes = new Uint8Array(binaryString.length);
            for (let i = 0; i < binaryString.length; i++) {
                bytes[i] = binaryString.charCodeAt(i);
            }
            
            const blob = new Blob([bytes], { type: json.data.mimeType || 'audio/wav' });
            return URL.createObjectURL(blob);
        } catch (err) {
            console.warn('Proxy audio fetch/decode failed:', err);
            return null;
        }
    }

    getPlayableUrl(url) {
        if (!url) return '';
        if (url.startsWith('blob:')) return url; // keep recorded blobs as-is
        const driveFileId = (() => {
            const fileMatch = url.match(/https?:\/\/drive\.google\.com\/file\/d\/([^/]+)\//i);
            if (fileMatch && fileMatch[1]) return fileMatch[1];
            const openMatch = url.match(/https?:\/\/drive\.google\.com\/open\?id=([^&]+)/i);
            if (openMatch && openMatch[1]) return openMatch[1];
            const ucMatch = url.match(/https?:\/\/drive\.google\.com\/uc\?id=([^&]+)/i);
            if (ucMatch && ucMatch[1]) return ucMatch[1];
            return null;
        })();
        if (driveFileId) {
            const appsScriptConfigured = this.appsScriptUrl &&
                this.appsScriptUrl !== 'YOUR_GOOGLE_APPS_SCRIPT_URL_HERE' &&
                this.appsScriptUrl !== 'YOUR_UNIVERSAL_APPS_SCRIPT_URL_HERE';

            if (appsScriptConfigured) {
                const proxyUrl = new URL(this.appsScriptUrl);
                proxyUrl.searchParams.set('action', 'getDriveFile');
                proxyUrl.searchParams.set('id', driveFileId);
                return proxyUrl.toString();
            }

            return `https://drive.google.com/uc?export=download&id=${driveFileId}`;
        }
        return url;
    }

    async fetchAsBlobUrl(url) {
        try {
            const response = await fetch(url, { mode: 'cors' });
            if (!response.ok) return null;
            const contentType = response.headers.get('content-type') || '';
            if (!contentType.includes('audio') && !contentType.includes('octet-stream')) return null;
            const blob = await response.blob();
            return URL.createObjectURL(blob);
        } catch (err) {
            console.warn('Audio fetch as blob failed, falling back to direct src:', err);
            return null;
        }
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
                this.updateSubmitButtonState();
                
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
        const acceptBtn = document.getElementById('accept-audio-btn');
        const fileInput = document.getElementById('audio-file-input');
        const fileNameDisplay = document.getElementById('file-name-display');
        
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
        if (acceptBtn) acceptBtn.style.display = '';
        if (fileInput) fileInput.value = '';
        if (fileNameDisplay) fileNameDisplay.textContent = '';
        
        this.recordedBlob = null;
        this.audioChunks = [];
    }

    async uploadAudioToDrive(blob) {
        if (!this.appsScriptUrl || 
            this.appsScriptUrl === 'YOUR_GOOGLE_APPS_SCRIPT_URL_HERE' ||
            this.appsScriptUrl === 'YOUR_UNIVERSAL_APPS_SCRIPT_URL_HERE') {
            throw new Error('Apps Script URL not configured');
        }
        
        // If recording is not m4a/mp3/wav, convert WebM (or other) to WAV before upload
        let uploadBlob = blob;
        let uploadMime = this.recordingMimeType || blob.type || 'audio/wav';
        if (!uploadMime.includes('mp4') && !uploadMime.includes('m4a') && !uploadMime.includes('mpeg') && !uploadMime.includes('mp3') && !uploadMime.includes('wav')) {
            uploadBlob = await this.convertBlobToWav(blob);
            uploadMime = 'audio/wav';
        }

        // Convert blob to base64
        const base64 = await this.blobToBase64(uploadBlob);
        
        // Generate filename with extension based on mime type (prefer mp3, else wav, else webm)
        const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
        const prefix = this.config.audio?.filePrefix || 'bowie-phone-recording';
        const mime = uploadMime || 'audio/wav';
        let extension = '.wav';
        if (mime.includes('mp4') || mime.includes('m4a')) extension = '.m4a';
        else if (mime.includes('mpeg') || mime.includes('mp3')) extension = '.mp3';
        else if (mime.includes('wav')) extension = '.wav';
        else {
            throw new Error('Unsupported audio format. Only M4A/MP3/WAV are allowed.');
        }
        const fileName = `${prefix}_${timestamp}${extension}`;
        
        // Enforce allowed mime types
        if (!mime.includes('mp4') && !mime.includes('m4a') && !mime.includes('mpeg') && !mime.includes('mp3') && !mime.includes('wav')) {
            throw new Error('Upload blocked: only M4A/MP3/WAV allowed.');
        }

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

    // Convert an arbitrary audio blob to WAV for upload compatibility
    async convertBlobToWav(blob) {
        const arrayBuffer = await blob.arrayBuffer();
        const audioCtx = new (window.AudioContext || window.webkitAudioContext)();
        const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);
        const wavBuffer = this.audioBufferToWav(audioBuffer);
        return new Blob([wavBuffer], { type: 'audio/wav' });
    }

    // Minimal PCM WAV encoder for an AudioBuffer
    audioBufferToWav(buffer) {
        const numChannels = buffer.numberOfChannels;
        const sampleRate = buffer.sampleRate;
        const samples = buffer.length;
        const bytesPerSample = 2; // 16-bit PCM
        const blockAlign = numChannels * bytesPerSample;
        const dataSize = samples * blockAlign;
        const bufferSize = 44 + dataSize;
        const arrayBuffer = new ArrayBuffer(bufferSize);
        const view = new DataView(arrayBuffer);

        let offset = 0;

        // RIFF header
        this.writeString(view, offset, 'RIFF'); offset += 4;
        view.setUint32(offset, 36 + dataSize, true); offset += 4; // file size - 8
        this.writeString(view, offset, 'WAVE'); offset += 4;

        // fmt chunk
        this.writeString(view, offset, 'fmt '); offset += 4;
        view.setUint32(offset, 16, true); offset += 4; // PCM chunk size
        view.setUint16(offset, 1, true); offset += 2;  // audio format PCM
        view.setUint16(offset, numChannels, true); offset += 2;
        view.setUint32(offset, sampleRate, true); offset += 4;
        view.setUint32(offset, sampleRate * blockAlign, true); offset += 4; // byte rate
        view.setUint16(offset, blockAlign, true); offset += 2; // block align
        view.setUint16(offset, bytesPerSample * 8, true); offset += 2; // bits per sample

        // data chunk
        this.writeString(view, offset, 'data'); offset += 4;
        view.setUint32(offset, dataSize, true); offset += 4;

        // Write interleaved PCM samples
        const channelData = [];
        for (let ch = 0; ch < numChannels; ch++) {
            channelData.push(buffer.getChannelData(ch));
        }

        let sampleIndex = 0;
        while (sampleIndex < samples) {
            for (let ch = 0; ch < numChannels; ch++) {
                const sample = Math.max(-1, Math.min(1, channelData[ch][sampleIndex]));
                view.setInt16(offset, sample * 0x7fff, true);
                offset += 2;
            }
            sampleIndex++;
        }

        return arrayBuffer;
    }

    writeString(view, offset, str) {
        for (let i = 0; i < str.length; i++) {
            view.setUint8(offset + i, str.charCodeAt(i));
        }
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

        if (!this.isValidUrl(link)) {
            this.showError('Please provide a valid audio/link URL.');
            this.updateSubmitButtonState();
            return;
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
        const status = document.getElementById('sync-status');
        const statusText = document.getElementById('sync-status-text');
        if (status) {
            status.classList.toggle('is-loading', show);
        }
        if (statusText && show) {
            statusText.textContent = 'Refreshing...';
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
