/**
 * Phone Dialer - Interactive DTMF dialer that triggers sequences
 * Uses Web Audio API for DTMF tone generation
 */
class PhoneDialer {
    // DTMF frequency pairs: [row_freq, col_freq]
    static DTMF_FREQS = {
        '1': [697, 1209], '2': [697, 1336], '3': [697, 1477],
        '4': [770, 1209], '5': [770, 1336], '6': [770, 1477],
        '7': [852, 1209], '8': [852, 1336], '9': [852, 1477],
        '*': [941, 1209], '0': [941, 1336], '#': [941, 1477],
    };

    static KEY_LABELS = {
        '1': '', '2': 'ABC', '3': 'DEF',
        '4': 'GHI', '5': 'JKL', '6': 'MNO',
        '7': 'PQRS', '8': 'TUV', '9': 'WXYZ',
        '*': '', '0': '+', '#': '',
    };

    constructor(app) {
        this.app = app;
        this.dialedNumber = '';
        this.audioCtx = null;
        this.isOpen = false;
        this.callActive = false;
        this.matchedSequence = null;
        this.ringbackTimeout = null;
        this._onKeyDown = this._handleKeyDown.bind(this);
        this._buildDOM();
    }

    _getAudioCtx() {
        if (!this.audioCtx) {
            this.audioCtx = new (window.AudioContext || window.webkitAudioContext)();
        }
        return this.audioCtx;
    }

    // --- DOM Construction ---

    _buildDOM() {
        // Backdrop
        this.backdrop = document.createElement('div');
        this.backdrop.className = 'dialer-backdrop';
        this.backdrop.addEventListener('click', () => this.close());

        // Dialog
        this.dialog = document.createElement('div');
        this.dialog.className = 'dialer-dialog';

        // Display area
        const display = document.createElement('div');
        display.className = 'dialer-display';
        this.numberDisplay = document.createElement('div');
        this.numberDisplay.className = 'dialer-number';
        this.numberDisplay.textContent = '\u00A0';
        this.matchDisplay = document.createElement('div');
        this.matchDisplay.className = 'dialer-match';
        display.appendChild(this.numberDisplay);
        display.appendChild(this.matchDisplay);

        // Status area (shown during calls)
        this.statusArea = document.createElement('div');
        this.statusArea.className = 'dialer-status';
        this.statusArea.style.display = 'none';

        // Keypad
        const keypad = document.createElement('div');
        keypad.className = 'dialer-keypad';
        const keyOrder = ['1','2','3','4','5','6','7','8','9','*','0','#'];
        keyOrder.forEach(key => {
            const btn = document.createElement('button');
            btn.className = 'dialer-key';
            btn.type = 'button';
            btn.dataset.key = key;
            btn.innerHTML = `<span class="dialer-key-digit">${key}</span><span class="dialer-key-letters">${PhoneDialer.KEY_LABELS[key]}</span>`;
            btn.addEventListener('pointerdown', (e) => {
                e.preventDefault();
                this._pressKey(key);
            });
            keypad.appendChild(btn);
        });

        // Action row
        const actions = document.createElement('div');
        actions.className = 'dialer-actions';

        this.backspaceBtn = document.createElement('button');
        this.backspaceBtn.className = 'dialer-action-btn dialer-backspace';
        this.backspaceBtn.type = 'button';
        this.backspaceBtn.innerHTML = '⌫';
        this.backspaceBtn.title = 'Backspace';
        this.backspaceBtn.addEventListener('click', () => this._backspace());

        this.callBtn = document.createElement('button');
        this.callBtn.className = 'dialer-action-btn dialer-call-btn';
        this.callBtn.type = 'button';
        this.callBtn.innerHTML = '📞';
        this.callBtn.title = 'Call';
        this.callBtn.addEventListener('click', () => this._toggleCall());

        this.clearBtn = document.createElement('button');
        this.clearBtn.className = 'dialer-action-btn dialer-clear';
        this.clearBtn.type = 'button';
        this.clearBtn.innerHTML = '✕';
        this.clearBtn.title = 'Clear';
        this.clearBtn.addEventListener('click', () => this._clear());

        actions.appendChild(this.backspaceBtn);
        actions.appendChild(this.callBtn);
        actions.appendChild(this.clearBtn);

        // Close button
        const closeBtn = document.createElement('button');
        closeBtn.className = 'dialer-close-btn';
        closeBtn.type = 'button';
        closeBtn.innerHTML = '×';
        closeBtn.title = 'Close';
        closeBtn.addEventListener('click', () => this.close());

        this.dialog.appendChild(closeBtn);
        this.dialog.appendChild(display);
        this.dialog.appendChild(this.statusArea);
        this.dialog.appendChild(keypad);
        this.dialog.appendChild(actions);

        document.body.appendChild(this.backdrop);
        document.body.appendChild(this.dialog);
    }

    // --- Open / Close ---

    open() {
        this._clear();
        this.backdrop.classList.add('show');
        this.dialog.classList.add('show');
        this.isOpen = true;
        document.addEventListener('keydown', this._onKeyDown);
    }

    close() {
        if (this.callActive) this._hangUp();
        this.backdrop.classList.remove('show');
        this.dialog.classList.remove('show');
        this.isOpen = false;
        document.removeEventListener('keydown', this._onKeyDown);
    }

    // --- Key Input ---

    _handleKeyDown(e) {
        if (!this.isOpen) return;
        if (e.key === 'Escape') { this.close(); return; }
        if (e.key === 'Backspace') { this._backspace(); e.preventDefault(); return; }
        if (e.key === 'Enter') { this._toggleCall(); e.preventDefault(); return; }
        const key = e.key;
        if (PhoneDialer.DTMF_FREQS[key]) {
            this._pressKey(key);
            e.preventDefault();
        }
    }

    _pressKey(key) {
        if (this.callActive) return;
        this._playDTMF(key);
        this.dialedNumber += key;
        this._updateDisplay();
        this._findMatch();

        // Visual feedback on keypad button
        const btn = this.dialog.querySelector(`.dialer-key[data-key="${key}"]`);
        if (btn) {
            btn.classList.add('active');
            setTimeout(() => btn.classList.remove('active'), 150);
        }
    }

    _backspace() {
        if (this.callActive) return;
        if (this.dialedNumber.length > 0) {
            this.dialedNumber = this.dialedNumber.slice(0, -1);
            this._updateDisplay();
            this._findMatch();
        }
    }

    _clear() {
        if (this.callActive) this._hangUp();
        this.dialedNumber = '';
        this.matchedSequence = null;
        this._updateDisplay();
        this.matchDisplay.textContent = '';
        this.matchDisplay.classList.remove('has-match', 'has-partial');
        this.callBtn.classList.remove('has-match', 'is-active');
        this.callBtn.innerHTML = '📞';
        this.statusArea.style.display = 'none';
    }

    // --- Display ---

    _updateDisplay() {
        this.numberDisplay.textContent = this.dialedNumber || '\u00A0';
        // Auto-shrink for long numbers
        if (this.dialedNumber.length > 12) {
            this.numberDisplay.style.fontSize = 'clamp(1.2rem, 5vw, 1.8rem)';
        } else {
            this.numberDisplay.style.fontSize = '';
        }
    }

    _findMatch() {
        const dialed = this.dialedNumber;
        if (!dialed) {
            this.matchDisplay.textContent = '';
            this.matchDisplay.classList.remove('has-match', 'has-partial');
            this.matchedSequence = null;
            this.callBtn.classList.remove('has-match');
            return;
        }

        const sequences = this.app.sequences || [];

        // Exact match
        const exact = sequences.find(s => s.number === dialed);
        if (exact) {
            this.matchedSequence = exact;
            this.matchDisplay.textContent = `📞 ${exact.name || exact.number}`;
            this.matchDisplay.classList.add('has-match');
            this.matchDisplay.classList.remove('has-partial');
            this.callBtn.classList.add('has-match');
            // Auto-call on exact match
            setTimeout(() => this._initiateCall(), 300);
            return;
        }

        // Partial matches
        const partials = sequences.filter(s => s.number && s.number.startsWith(dialed));
        this.matchedSequence = null;
        this.callBtn.classList.remove('has-match');
        if (partials.length > 0) {
            const hint = partials.length === 1
                ? `… ${partials[0].name || partials[0].number}`
                : `${partials.length} matches`;
            this.matchDisplay.textContent = hint;
            this.matchDisplay.classList.add('has-partial');
            this.matchDisplay.classList.remove('has-match');
        } else {
            this.matchDisplay.textContent = dialed.length >= 1 ? 'No match' : '';
            this.matchDisplay.classList.remove('has-match', 'has-partial');
        }
    }

    // --- Call / Hang-up ---

    _toggleCall() {
        if (this.callActive) {
            this._hangUp();
        } else {
            this._initiateCall();
        }
    }

    _initiateCall() {
        if (!this.dialedNumber) return;

        // Try to find an exact match if we don't already have one
        if (!this.matchedSequence) {
            const exact = (this.app.sequences || []).find(s => s.number === this.dialedNumber);
            if (exact) this.matchedSequence = exact;
        }

        if (!this.matchedSequence) {
            this.statusArea.textContent = '❌ Number not found';
            this.statusArea.style.display = 'block';
            setTimeout(() => { this.statusArea.style.display = 'none'; }, 2000);
            return;
        }

        this.callActive = true;
        this.callBtn.classList.add('is-active');
        this.callBtn.innerHTML = '📵';
        this.callBtn.title = 'Hang up';

        const seq = this.matchedSequence;
        const ringDuration = parseInt(seq.ring_duration, 10) || 0;

        this.statusArea.textContent = ringDuration > 0 ? '🔔 Ringing…' : '📞 Connected';
        this.statusArea.style.display = 'block';

        if (ringDuration > 0) {
            this.ringbackTimeout = setTimeout(() => {
                if (!this.callActive) return;
                this.statusArea.textContent = '📞 Connected';
                this._playSequenceAudio(seq);
            }, ringDuration);
        } else {
            this._playSequenceAudio(seq);
        }
    }

    _playSequenceAudio(seq) {
        if (!seq.link) {
            this.statusArea.textContent = '🔇 No audio available';
            return;
        }
        // Delegate to the main app's playback (it handles Drive proxy, blob URLs, etc.)
        this.app.playSequenceLink(seq.id);
        this.statusArea.textContent = `▶️ Playing: ${seq.name || seq.number}`;
    }

    _hangUp() {
        this.callActive = false;
        if (this.ringbackTimeout) {
            clearTimeout(this.ringbackTimeout);
            this.ringbackTimeout = null;
        }
        // Stop any playing audio
        this.app.resetListPlaybackUI();
        this.callBtn.classList.remove('is-active');
        this.callBtn.innerHTML = '📞';
        this.callBtn.title = 'Call';
        this.statusArea.textContent = '📴 Call ended';
        setTimeout(() => {
            if (!this.callActive) this.statusArea.style.display = 'none';
        }, 1500);
    }

    // --- DTMF Tone Generation (Web Audio API) ---

    _playDTMF(key) {
        const freqs = PhoneDialer.DTMF_FREQS[key];
        if (!freqs) return;

        const ctx = this._getAudioCtx();
        const duration = 0.15;
        const now = ctx.currentTime;

        const gain = ctx.createGain();
        gain.gain.setValueAtTime(0.2, now);
        gain.gain.exponentialRampToValueAtTime(0.001, now + duration);
        gain.connect(ctx.destination);

        freqs.forEach(freq => {
            const osc = ctx.createOscillator();
            osc.type = 'sine';
            osc.frequency.setValueAtTime(freq, now);
            osc.connect(gain);
            osc.start(now);
            osc.stop(now + duration);
        });
    }
}
