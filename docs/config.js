// Bowie Phone Sequence Manager Configuration
// Customize this file to match your project needs

const PHONE_CONFIG = {
    // Project identification
    projectName: "Bowie Phone Sequences",
    projectId: "bowie-phone-sequences",
    
    // Debug mode - set to true to enable debug console and logging
    debug: true,
    
    // Google Sheets Integration
    // Set enabled: false to disable Google Sheets and use local storage only
    // IMPORTANT: Copy config.example.js to config.local.js and fill in your real values
    googleSheets: {
        enabled: true,
        gid: '0',
        csvUrl: null, // Auto-constructed
        // Use the universal backend (shared across all apps)
        // OR use a dedicated bowie-phone-apps-script.js deployment
        // Must include /exec at the end to hit the deployed web app
        appScriptUrl: 'https://script.google.com/macros/s/AKfycbwA9YLBm1Q4hrLwzlq8GF2kiQippL5_SkFB0shJA8U6REI2F6CYj4mD0XCRXd86SMps/exec'
    },
    
    // Google Drive folder for audio uploads
    googleDrive: {
        folderId: 'YOUR_DRIVE_FOLDER_ID_HERE',
        folderUrl: 'https://drive.google.com/drive/folders/YOUR_DRIVE_FOLDER_ID_HERE'
    },
    
    // Data source configuration
    dataSource: {
        localStorageKey: "bowiePhone_sequences",
        enableLocalStorage: false, // Force Google Sheets only
        syncInterval: 30000, // 30 seconds
        enableOfflineMode: true
    },
    
    // Sequence types available for the Link column
    sequenceTypes: [
        { value: 'audio', label: 'Audio File', icon: '🎵' },
        { value: 'url', label: 'Web URL', icon: '🔗' },
        { value: 'shortcut', label: 'Shortcut Command', icon: '⌘' }
    ],
    
    // Predefined special number names (non-digit sequences)
    specialNumbers: [
        'dialtone',
        'busy',
        'ringback',
        'disconnect',
        'error',
        'silence'
    ],
    
    // Data field configuration
    data: {
        name: {
            type: 'text',
            required: true,
            label: 'Name',
            icon: '📛',
            placeholder: 'Description of this sequence...'
        },
        number: {
            type: 'text',
            required: true,
            label: 'Number',
            icon: '📞',
            placeholder: 'e.g., 911, *123#, or dialtone',
            validate: true
        },
        link: {
            type: 'audio',
            required: false,
            label: 'Link',
            icon: '🔗',
            placeholder: 'Audio URL or recording...'
        }
    },
    
    // Sorting configuration
    sort: [
        {
            field: 'number',
            direction: 'asc'
        }
    ],
    
    // UI Configuration
    ui: {
        defaultFilter: "all",
        showProgressBar: false,
        showSyncStatus: true,
        
        theme: {
            colorPrimary: "#00ff88",
            colorBackground: "#0d1117",
            colorText: "#f0f6fc"
        }
    },
    
    // Features configuration
    features: {
        allowCreation: true,
        allowEditing: true,
        allowDeletion: true
    },
    
    // Audio recording configuration
    audio: {
        // Maximum recording duration in seconds
        maxDuration: 300, // 5 minutes
        
        // Audio format settings
        mimeType: 'audio/webm;codecs=opus',
        fallbackMimeType: 'audio/webm',
        
        // File naming
        filePrefix: 'bowie-phone-recording'
    }
};

// Export for use in app.js
window.PHONE_CONFIG = PHONE_CONFIG;

// Try to load local config overrides if available
(function() {
    const script = document.createElement('script');
    script.src = 'config.local.js';
    script.onload = function() {
        if (typeof LOCAL_PHONE_CONFIG !== 'undefined') {
            // Deep merge local config
            function deepMerge(target, source) {
                for (const key in source) {
                    if (source[key] && typeof source[key] === 'object' && !Array.isArray(source[key])) {
                        if (!target[key]) target[key] = {};
                        deepMerge(target[key], source[key]);
                    } else {
                        target[key] = source[key];
                    }
                }
            }
            deepMerge(PHONE_CONFIG, LOCAL_PHONE_CONFIG);
            console.log('✅ Local config loaded and merged');
        }
        // Dispatch event when config is ready
        window.dispatchEvent(new CustomEvent('configReady', { detail: PHONE_CONFIG }));
    };
    script.onerror = function() {
        console.log('ℹ️ No local config found, using defaults');
        // Still dispatch event even without local config
        window.dispatchEvent(new CustomEvent('configReady', { detail: PHONE_CONFIG }));
    };
    document.head.appendChild(script);
})();
