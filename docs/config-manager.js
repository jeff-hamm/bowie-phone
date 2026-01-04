/**
 * Multi-tenant Configuration Manager
 * Loads different configurations based on the hostname
 */

// Define configurations for each hostname/site OR by cookie name
const SITE_CONFIGS = {
    // Default configuration (fallback) - also used for 'bowie' cookie
    'default': {
        projectName: "Bowie Phone Sequences",
        projectId: "bowie-phone-sequences",
        theme: "default",
        googleSheets: {
            enabled: true,
            gid: '0',
            spreadsheetId: '1q1FOzSTg-5ATMSlVf_cuIaPvYgMv9yDec4Nd5ZaAlzY',
            appScriptUrl: 'https://script.google.com/macros/s/AKfycbwA9YLBm1Q4hrLwzlq8GF2kiQippL5_SkFB0shJA8U6REI2F6CYj4mD0XCRXd86SMps/exec'
        },
        googleDrive: {
            folderId: '1TGRbklQEoJpt1AbR3LOpW0t8unVnzkQ5',
            folderUrl: 'https://drive.google.com/drive/folders/1TGRbklQEoJpt1AbR3LOpW0t8unVnzkQ5'
        },
        customStyles: {
            primaryColor: '#4CAF50',
            accentColor: '#2196F3'
        }
    },
    
    // Bowie Phone configuration (cookie: bowie)
    'bowie': {
        projectName: "Bowie Phone Sequences",
        projectId: "bowie-phone-sequences",
        theme: "default",
        googleSheets: {
            enabled: true,
            gid: '0',
            spreadsheetId: '1q1FOzSTg-5ATMSlVf_cuIaPvYgMv9yDec4Nd5ZaAlzY',
            appScriptUrl: 'https://script.google.com/macros/s/AKfycbwA9YLBm1Q4hrLwzlq8GF2kiQippL5_SkFB0shJA8U6REI2F6CYj4mD0XCRXd86SMps/exec'
        },
        googleDrive: {
            folderId: '1TGRbklQEoJpt1AbR3LOpW0t8unVnzkQ5',
            folderUrl: 'https://drive.google.com/drive/folders/1TGRbklQEoJpt1AbR3LOpW0t8unVnzkQ5'
        },
        customStyles: {
            primaryColor: '#4CAF50',
            accentColor: '#2196F3'
        }
    },
    
    // BroPhone configuration (standalone site)
    'brophone.infinitebutts.com': {
        projectName: "BroPhone Sequences",
        projectId: "brophone-sequences",
        theme: "default",
        googleSheets: {
            enabled: true,
            gid: '0',
            spreadsheetId: '1lRFBYeUsTFvTeIVyCezVcGScwPJVZT5jfsVv7zJbQdo',
            appScriptUrl: 'https://script.google.com/macros/s/AKfycbwA9YLBm1Q4hrLwzlq8GF2kiQippL5_SkFB0shJA8U6REI2F6CYj4mD0XCRXd86SMps/exec'
        },
        googleDrive: {
            folderId: '1UFu18QqcmKmuNtBYLL6NtwPxlUBw6zYK',
            folderUrl: 'https://drive.google.com/drive/folders/1UFu18QqcmKmuNtBYLL6NtwPxlUBw6zYK?usp=sharing'
        },
        customStyles: {
            primaryColor: '#2196F3',
            accentColor: '#1976D2'
        }
    },
    
    // Bowie Phone configuration (standalone site)
    'bowie-phone.infinitebutts.com': {
        projectName: "Bowie Phone Sequences",
        projectId: "bowie-phone-sequences",
        theme: "default",
        googleSheets: {
            enabled: true,
            gid: '0',
            spreadsheetId: '1q1FOzSTg-5ATMSlVf_cuIaPvYgMv9yDec4Nd5ZaAlzY',
            appScriptUrl: 'https://script.google.com/macros/s/AKfycbwA9YLBm1Q4hrLwzlq8GF2kiQippL5_SkFB0shJA8U6REI2F6CYj4mD0XCRXd86SMps/exec'
        },
        googleDrive: {
            folderId: '1TGRbklQEoJpt1AbR3LOpW0t8unVnzkQ5',
            folderUrl: 'https://drive.google.com/drive/folders/1TGRbklQEoJpt1AbR3LOpW0t8unVnzkQ5'
        },
        customStyles: {
            primaryColor: '#4CAF50',
            accentColor: '#2196F3'
        }
    },
    
    // You can also match by localhost ports for development
    'localhost:8000': {
        projectName: "Dev Environment (Port 8000)",
        projectId: "dev-8000",
        theme: "default",
        googleSheets: {
            enabled: true,
            gid: '0',
            spreadsheetId: 'DEV_SPREADSHEET_ID',
            appScriptUrl: 'DEV_APPS_SCRIPT_URL'
        },
        googleDrive: {
            folderId: 'DEV_DRIVE_FOLDER_ID',
            folderUrl: 'https://drive.google.com/drive/folders/DEV_DRIVE_FOLDER_ID'
        }
    }
};

class ConfigManager {
    constructor() {
        this.hostname = window.location.hostname;
        this.port = window.location.port;
        this.fullHost = this.port ? `${this.hostname}:${this.port}` : this.hostname;
        this.cookieName = 'phone-config';
        this.sessionKey = 'phone-config-selected';
        this.config = null;
        this.needsSelection = false;
    }
    
    isDirectAccess() {
        // Check if user came directly to main site without a config
        const cookieConfig = this.getCookie(this.cookieName);
        const sessionSelected = sessionStorage.getItem(this.sessionKey);
        const referrer = document.referrer;
        
        // If cookie exists or already selected in session, no need to show selector
        if (cookieConfig || sessionSelected) {
            return false;
        }
        
        // Check if referrer is from one of our redirect sites
        const redirectSites = ['bowie-phone.infinitebutts.com', 'brophone.infinitebutts.com'];
        const cameFromRedirect = redirectSites.some(site => referrer.includes(site));
        
        // If on main site, no cookie, not from redirect = direct access
        const isMainSite = this.hostname === 'phone.infinitebutts.com' || 
                          this.hostname === 'localhost' ||
                          this.fullHost.startsWith('localhost:');
        
        return isMainSite && !cameFromRedirect;
    }
    
    getCookie(name) {
        const value = `; ${document.cookie}`;
        const parts = value.split(`; ${name}=`);
        if (parts.length === 2) {
            return parts.pop().split(';').shift();
        }
        return null;
    }
    
    setCookie(name, value, days = 365) {
        const expires = new Date();
        expires.setTime(expires.getTime() + (days * 24 * 60 * 60 * 1000));
        document.cookie = `${name}=${value};expires=${expires.toUTCString()};path=/;SameSite=Lax`;
    }
    
    handleRedirect() {
        // Check if this hostname should redirect
        const hostConfig = SITE_CONFIGS[this.fullHost] || SITE_CONFIGS[this.hostname];
        
        if (hostConfig && hostConfig.redirectTo && hostConfig.setCookie) {
            console.log(`🔄 Redirect site detected: ${this.hostname}`);
            console.log(`📝 Setting cookie: ${this.cookieName}=${hostConfig.setCookie}`);
            console.log(`➡️  Redirecting to: ${hostConfig.redirectTo}`);
            
            // Set the cookie
            this.setCookie(this.cookieName, hostConfig.setCookie);
            
            // Redirect to the main site
            const protocol = window.location.protocol;
            window.location.href = `${protocol}//${hostConfig.redirectTo}`;
            return true;
        }
        return false;
    }

    loadConfig() {
        console.log(`🌐 Loading configuration for: ${this.fullHost}`);
        
        // Check if this is a redirect hostname
        if (this.handleRedirect()) {
            // Redirect is happening, return a minimal config
            return this.mergeWithDefaults(SITE_CONFIGS['default']);
        }
        
        // Check if user needs to select a config
        if (this.isDirectAccess()) {
            console.log(`🎯 Direct access detected - will show config selector`);
            this.needsSelection = true;
            // Return default for now, will update after selection
            return this.mergeWithDefaults(SITE_CONFIGS['default']);
        }
        
        // Check for cookie-based configuration first
        const cookieConfig = this.getCookie(this.cookieName);
        if (cookieConfig && SITE_CONFIGS[cookieConfig]) {
            console.log(`🍪 Found cookie configuration: ${cookieConfig}`);
            return this.mergeWithDefaults(SITE_CONFIGS[cookieConfig]);
        }
        
        // Check session storage (in case cookie was cleared but session active)
        const sessionConfig = sessionStorage.getItem(this.sessionKey);
        if (sessionConfig && SITE_CONFIGS[sessionConfig]) {
            console.log(`💾 Found session configuration: ${sessionConfig}`);
            return this.mergeWithDefaults(SITE_CONFIGS[sessionConfig]);
        }
        
        // Try to find exact match with port
        if (SITE_CONFIGS[this.fullHost]) {
            const config = SITE_CONFIGS[this.fullHost];
            // Check if this config wants to use cookie
            if (config.useCookie && !cookieConfig) {
                console.log(`⚠️  Hostname ${this.fullHost} expects cookie but none found, using default`);
                return this.mergeWithDefaults(SITE_CONFIGS['default']);
            }
            console.log(`✅ Found configuration for: ${this.fullHost}`);
            return this.mergeWithDefaults(config);
        }
        
        // Try hostname without port
        if (SITE_CONFIGS[this.hostname]) {
            const config = SITE_CONFIGS[this.hostname];
            // Check if this config wants to use cookie
            if (config.useCookie && !cookieConfig) {
                console.log(`⚠️  Hostname ${this.hostname} expects cookie but none found, using default`);
                return this.mergeWithDefaults(SITE_CONFIGS['default']);
            }
            console.log(`✅ Found configuration for: ${this.hostname}`);
            return this.mergeWithDefaults(config);
        }
        
        // Try wildcard subdomain matching (e.g., *.example.com)
        const wildcardMatch = this.findWildcardMatch();
        if (wildcardMatch) {
            console.log(`✅ Found wildcard configuration for: ${this.hostname}`);
            return this.mergeWithDefaults(wildcardMatch);
        }
        
        // Fall back to default
        console.log(`⚠️  No specific configuration found, using default`);
        return this.mergeWithDefaults(SITE_CONFIGS['default']);
    }

    findWildcardMatch() {
        for (const [pattern, config] of Object.entries(SITE_CONFIGS)) {
            if (pattern.startsWith('*.')) {
                const domain = pattern.substring(2); // Remove *.
                if (this.hostname.endsWith(domain)) {
                    return config;
                }
            }
        }
        return null;
    }

    mergeWithDefaults(siteConfig) {
        // Deep merge with default config
        const defaultConfig = SITE_CONFIGS['default'];
        return {
            debug: siteConfig.debug ?? defaultConfig.debug ?? false,
            projectName: siteConfig.projectName || defaultConfig.projectName,
            projectId: siteConfig.projectId || defaultConfig.projectId,
            theme: siteConfig.theme || defaultConfig.theme || 'default',
            
            googleSheets: {
                ...defaultConfig.googleSheets,
                ...siteConfig.googleSheets
            },
            
            googleDrive: {
                ...defaultConfig.googleDrive,
                ...siteConfig.googleDrive
            },
            
            customStyles: {
                ...defaultConfig.customStyles,
                ...siteConfig.customStyles
            },
            
            dataSource: siteConfig.dataSource || defaultConfig.dataSource || {
                localStorageKey: `${siteConfig.projectId}_sequences`,
                enableLocalStorage: false,
                syncInterval: 30000,
                enableOfflineMode: true
            },
            
            sequenceTypes: siteConfig.sequenceTypes || defaultConfig.sequenceTypes || [
                { value: 'audio', label: 'Audio File', icon: '🎵' },
                { value: 'url', label: 'Web URL', icon: '🔗' },
                { value: 'shortcut', label: 'Shortcut Command', icon: '⌘' }
            ],
            
            specialNumbers: siteConfig.specialNumbers || defaultConfig.specialNumbers || [
                'dialtone', 'busy', 'error', 'ringback'
            ]
        };
    }

    applyTheme() {
        const theme = this.config.theme;
        console.log(`🎨 Applying theme: ${theme}`);
        
        // Remove any existing theme stylesheets
        const existingTheme = document.getElementById('theme-stylesheet');
        if (existingTheme) {
            existingTheme.remove();
        }
        
        // Add new theme stylesheet if not default
        if (theme && theme !== 'default') {
            const link = document.createElement('link');
            link.id = 'theme-stylesheet';
            link.rel = 'stylesheet';
            link.href = `styles/theme-${theme}.css`;
            document.head.appendChild(link);
        }
        
        // Apply custom CSS variables if provided
        if (this.config.customStyles) {
            const root = document.documentElement;
            if (this.config.customStyles.primaryColor) {
                root.style.setProperty('--primary-color', this.config.customStyles.primaryColor);
            }
            if (this.config.customStyles.accentColor) {
                root.style.setProperty('--accent-color', this.config.customStyles.accentColor);
            }
        }
    }

    updatePageTitle() {
        document.title = this.config.projectName || 'Phone Sequence Manager';
        
        // Update h1 if it exists
        const h1 = document.querySelector('h1');
        if (h1 && !h1.dataset.keepOriginal) {
            h1.textContent = this.config.projectName.replace(' Sequences', '');
        }
    }

    getConfig() {
        return this.config;
    }

    // Helper method to get just the PHONE_CONFIG format for backward compatibility
    getPhoneConfig() {
        return {
            projectName: this.config.projectName,
            projectId: this.config.projectId,
            debug: this.config.debug,
            googleSheets: this.config.googleSheets,
            googleDrive: this.config.googleDrive,
            dataSource: this.config.dataSource,
            sequenceTypes: this.config.sequenceTypes,
            specialNumbers: this.config.specialNumbers
        };
    }
    
    showConfigSelector() {
        // Create modal overlay
        const overlay = document.createElement('div');
        overlay.id = 'config-selector-overlay';
        overlay.style.cssText = `
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background: rgba(0, 0, 0, 0.7);
            display: flex;
            align-items: center;
            justify-content: center;
            z-index: 10000;
            animation: fadeIn 0.3s;
        `;
        
        // Create selector card
        const card = document.createElement('div');
        card.style.cssText = `
            background: white;
            border-radius: 16px;
            padding: 2.5rem;
            max-width: 500px;
            width: 90%;
            box-shadow: 0 20px 60px rgba(0, 0, 0, 0.3);
            animation: slideUp 0.3s;
        `;
        
        card.innerHTML = `
            <style>
                @keyframes fadeIn {
                    from { opacity: 0; }
                    to { opacity: 1; }
                }
                @keyframes slideUp {
                    from { transform: translateY(20px); opacity: 0; }
                    to { transform: translateY(0); opacity: 1; }
                }
                .config-selector-title {
                    font-size: 1.8rem;
                    font-weight: 600;
                    color: #333;
                    margin-bottom: 0.5rem;
                    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
                }
                .config-selector-subtitle {
                    color: #666;
                    margin-bottom: 2rem;
                    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
                }
                .config-option {
                    display: flex;
                    align-items: center;
                    padding: 1.2rem;
                    margin-bottom: 1rem;
                    border: 2px solid #e0e0e0;
                    border-radius: 12px;
                    cursor: pointer;
                    transition: all 0.3s;
                    background: white;
                }
                .config-option:hover {
                    border-color: #667eea;
                    background: #f8f9ff;
                    transform: translateX(4px);
                }
                .config-option-icon {
                    font-size: 2.5rem;
                    margin-right: 1rem;
                }
                .config-option-info {
                    flex: 1;
                }
                .config-option-name {
                    font-weight: 600;
                    font-size: 1.1rem;
                    color: #333;
                    margin-bottom: 0.25rem;
                    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
                }
                .config-option-desc {
                    color: #666;
                    font-size: 0.9rem;
                    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
                }
            </style>
            <h2 class="config-selector-title">📞 Select Your Phone System</h2>
            <p class="config-selector-subtitle">Choose which phone configuration to use:</p>
            
            <div class="config-option" onclick="configManager.selectConfig('bowie')">
                <div class="config-option-icon">📞</div>
                <div class="config-option-info">
                    <div class="config-option-name">Bowie Phone</div>
                    <div class="config-option-desc">Green theme • Original sequences</div>
                </div>
            </div>
            
            <div class="config-option" onclick="configManager.selectConfig('brophone')">
                <div class="config-option-icon">📱</div>
                <div class="config-option-info">
                    <div class="config-option-name">BroPhone</div>
                    <div class="config-option-desc">Blue theme • Alternative sequences</div>
                </div>
            </div>
        `;
        
        overlay.appendChild(card);
        document.body.appendChild(overlay);
    }
    
    selectConfig(configName) {
        console.log(`✅ User selected configuration: ${configName}`);
        
        // Set cookie
        this.setCookie(this.cookieName, configName);
        
        // Set session storage
        sessionStorage.setItem(this.sessionKey, configName);
        
        // Reload page to apply new config
        window.location.reload();
    }
    
    init() {
        // Load configuration
        this.config = this.loadConfig();
        
        // Show selector if needed
        if (this.needsSelection) {
            // Wait for DOM to be ready
            if (document.readyState === 'loading') {
                document.addEventListener('DOMContentLoaded', () => {
                    this.showConfigSelector();
                });
            } else {
                this.showConfigSelector();
            }
        } else {
            // Apply theme and update page as normal
            this.applyTheme();
            if (document.readyState === 'loading') {
                document.addEventListener('DOMContentLoaded', () => {
                    this.updatePageTitle();
                });
            } else {
                this.updatePageTitle();
            }
        }
    }
}

// Create global instance
const configManager = new ConfigManager();

// Initialize (will show selector if needed, or apply theme/config)
configManager.init();

// Export for use in app.js (backward compatible with PHONE_CONFIG)
// Note: If selector is shown, this will be default config initially
const PHONE_CONFIG = configManager.getPhoneConfig();

console.log('📋 Active configuration:', PHONE_CONFIG);
