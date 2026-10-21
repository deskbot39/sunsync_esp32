const API_BASE_URL = (typeof window.SUNSYNC_API_BASE === 'string'
    ? window.SUNSYNC_API_BASE
    : 'http://localhost:8000').replace(/\/$/, '');
const DEVICE_API_BASE_URL = '';
const DEVICE_STORAGE_KEY = 'sunSyncDeviceId';
const SETTINGS_ID = 1;
const PAYMENT_POLL_INTERVAL_MS = 1000;
let countdownTimer;
let paymentPollTimer;
let activeSession = null;
let paymentStateAvailable = false;
let coinInputEnabled = false;
let creditBalance = 0;
let serviceRates = null;
let serviceActionPending = false;

const payButton = document.querySelector('.btn-pay');
const connectButton = document.querySelector('.btn-connect');
const chargeButton = document.querySelector('.btn-charge');
const pauseButton = document.querySelector('.btn-pause');
const creditBalanceElement = document.querySelector('#credit-balance');
const creditHelpElement = document.querySelector('#credit-help');
const actionStatusElement = document.querySelector('.action-status');
const sessionClock = document.querySelector('.coin-status');
const sessionClockText = document.querySelector('.coin-clock');

function getDeviceId() {
	let deviceId = localStorage.getItem(DEVICE_STORAGE_KEY);
	if (!deviceId) {
		deviceId = crypto.randomUUID();
		localStorage.setItem(DEVICE_STORAGE_KEY, deviceId);
	}
	return deviceId;
}

async function registerDevice() {
	const deviceId = getDeviceId();
	const response = await fetch(
		`${API_BASE_URL}/devices/register?device_id=${encodeURIComponent(deviceId)}`,
		{ method: 'POST' }
	);

	if (!response.ok) {
		throw new Error(`Device registration failed (${response.status})`);
	}

	return response.json();
}

function formatDuration(totalSeconds) {
	const seconds = Math.max(0, Math.floor(totalSeconds));
	const hours = Math.floor(seconds / 3600);
	const minutes = Math.floor((seconds % 3600) / 60);
	const remainder = seconds % 60;
	return [hours, minutes, remainder].map(value => String(value).padStart(2, '0')).join(':');
}

function renderRates(rates) {
	document.querySelectorAll('[data-denomination]').forEach(row => {
		const denomination = Number(row.dataset.denomination);
		row.querySelector('.wifi-time').textContent = formatDuration(rates.wifi_rate_per_coin * denomination);
		row.querySelector('.charging-time').textContent = formatDuration(rates.charging_rate_per_coin * denomination);
	});
}

function isSessionAvailable(session) {
	if (!session || session.session_active !== true || !session.expires_at) {
		return false;
	}
	return Boolean(session.paused_at) || Date.parse(session.expires_at) > Date.now();
}

function renderSession(session) {
	clearInterval(countdownTimer);
	activeSession = isSessionAvailable(session) ? session : null;
	pauseButton.disabled = !activeSession;
	pauseButton.textContent = activeSession?.paused_at ? 'Resume Time' : 'Pause Time';

	if (!activeSession) {
		sessionClockText.textContent = '00:00:00';
		sessionClock.style.setProperty('--timer-progress', '0%');
		sessionClock.classList.remove('is-active', 'is-paused');
		sessionClock.setAttribute('aria-label', 'No active session');
		updateServiceButtons();
		return;
	}

	const updateTimer = () => {
		let remaining;
		if (activeSession?.paused_at) {
			remaining = Math.max(
				0,
				(Date.parse(activeSession.expires_at) - Date.parse(activeSession.paused_at)) / 1000
			);
		} else {
			remaining = Math.max(
				0,
				(Date.parse(activeSession.expires_at) - Date.now()) / 1000
			);
		}

		const totalDuration = Number.isSafeInteger(activeSession.duration_seconds) &&
			activeSession.duration_seconds > 0
			? activeSession.duration_seconds
			: Math.max(remaining, (Date.parse(activeSession.expires_at) -
				Date.parse(activeSession.session_start)) / 1000);
		const progress = totalDuration > 0
			? Math.min(100, Math.max(0, remaining / totalDuration * 100))
			: 0;
		const formattedRemaining = formatDuration(remaining);
		sessionClockText.textContent = formattedRemaining;
		sessionClock.style.setProperty('--timer-progress', `${progress}%`);
		sessionClock.classList.toggle('is-paused', Boolean(activeSession.paused_at));
		sessionClock.classList.toggle('is-active', !activeSession.paused_at);
		sessionClock.setAttribute(
			'aria-label',
			`${activeSession.paused_at ? 'Paused' : 'Active'} ${activeSession.session_type} session, ${formattedRemaining} remaining`
		);
		if (remaining <= 0) {
			renderSession(null);
		}
	};

	updateTimer();
	if (activeSession) {
		countdownTimer = setInterval(updateTimer, 1000);
	}
	updateServiceButtons();
}

function updateServiceButtons() {
	const hasActiveSession = Boolean(activeSession);
	const canStart = paymentStateAvailable && creditBalance > 0 &&
		!hasActiveSession && !serviceActionPending;
	const wifiRateAvailable = Number.isSafeInteger(serviceRates?.wifi_rate_per_coin) &&
		serviceRates.wifi_rate_per_coin > 0;
	const chargingRateAvailable = Number.isSafeInteger(serviceRates?.charging_rate_per_coin) &&
		serviceRates.charging_rate_per_coin > 0;
	connectButton.disabled = !canStart ||
		!wifiRateAvailable;
	chargeButton.disabled = !canStart ||
		!chargingRateAvailable;
}

function renderPaymentStatus(status) {
	paymentStateAvailable = true;
	coinInputEnabled = status.coin_input_enabled === true;
	creditBalance = Number.isSafeInteger(status.credits) && status.credits > 0
		? status.credits
		: 0;
	const windowActive = status.pay_window_active === true;
	const secondsRemaining = Number.isSafeInteger(status.seconds_remaining)
		? Math.max(0, status.seconds_remaining)
		: 0;

	creditBalanceElement.textContent = String(creditBalance);
	payButton.disabled = !coinInputEnabled || windowActive;
	updateServiceButtons();

	if (!coinInputEnabled) {
		creditHelpElement.textContent =
			'Coin input is disabled until a safe 3.3 V signal interface is connected and enabled.';
	} else if (windowActive) {
		creditHelpElement.textContent =
			`Payment window open: ${secondsRemaining}s. Each accepted coin-signal pulse adds 1 credit.`;
	} else if (creditBalance > 0) {
		creditHelpElement.textContent =
			'Credits are counted as signal pulses; the acceptor denomination mapping is not configured.';
	} else {
		creditHelpElement.textContent =
			'Tap Pay to open a 60-second window. Each accepted coin-signal pulse adds 1 credit.';
	}
}

function renderPaymentUnavailable(error) {
	if (error) {
		console.error(error);
	}
	paymentStateAvailable = false;
	coinInputEnabled = false;
	creditBalance = 0;
	creditBalanceElement.textContent = '0';
	payButton.disabled = true;
	updateServiceButtons();
	creditHelpElement.textContent = 'Payment controller is unavailable; no credits can be accepted.';
}

async function fetchRates() {
	const response = await fetch(`${API_BASE_URL}/settings/public/${SETTINGS_ID}`);
	if (!response.ok) throw new Error(`Rates request failed (${response.status})`);
	serviceRates = await response.json();
	renderRates(serviceRates);
	updateServiceButtons();
}

async function fetchActiveSession() {
	const deviceId = getDeviceId();
	const response = await fetch(
		`${API_BASE_URL}/sessions/device/${encodeURIComponent(deviceId)}/active`
	);
	if (!response.ok) throw new Error(`Session request failed (${response.status})`);
	renderSession(await response.json());
}

async function fetchPaymentStatus() {
	const deviceId = encodeURIComponent(getDeviceId());
	const response = await fetch(
		`${DEVICE_API_BASE_URL}/api/payment/status?device_id=${deviceId}`,
		{ cache: 'no-store' }
	);
	if (!response.ok) {
		throw new Error(`Payment status request failed (${response.status})`);
	}
	renderPaymentStatus(await response.json());
}

async function openPaymentWindow() {
	if (!paymentStateAvailable || !coinInputEnabled || payButton.disabled) {
		return;
	}
	payButton.disabled = true;
	actionStatusElement.textContent = '';
	try {
		const deviceId = encodeURIComponent(getDeviceId());
		const response = await fetch(
			`${DEVICE_API_BASE_URL}/api/payment/open?device_id=${deviceId}`,
			{ method: 'POST' }
		);
		if (!response.ok) {
			const result = await response.json();
			throw new Error(result.detail || `Could not open payment window (${response.status})`);
		}
		renderPaymentStatus(await response.json());
	} catch (error) {
		console.error(error);
		actionStatusElement.textContent = error.message;
		await fetchPaymentStatus().catch(renderPaymentUnavailable);
	}
}

async function toggleSessionPause() {
	if (!isSessionAvailable(activeSession)) {
		renderSession(null);
		return;
	}
	if (!Number.isSafeInteger(activeSession.id)) {
		throw new Error('This session cannot be paused from the current page.');
	}
	const action = activeSession.paused_at ? 'resume' : 'pause';
	const deviceId = encodeURIComponent(getDeviceId());
	if (action === 'pause') {
		await requestDeviceService('pause', getDeviceId(), activeSession.session_type);
	}
	const response = await fetch(
		`${API_BASE_URL}/sessions/${activeSession.id}/${action}?device_id=${deviceId}`,
		{ method: 'PATCH' }
	);
	if (!response.ok) {
		if (action === 'pause') {
			await requestDeviceService('resume', getDeviceId(), activeSession.session_type)
				.catch(error => console.error('Could not restore device session after pause failed.', error));
		}
		throw new Error(`Could not ${action} session (${response.status})`);
	}
	const updatedSession = await response.json();
	if (action === 'resume') {
		try {
			await requestDeviceService('resume', getDeviceId(), updatedSession.session_type);
		} catch (error) {
			await fetch(
				`${API_BASE_URL}/sessions/${updatedSession.id}/pause?device_id=${deviceId}`,
				{ method: 'PATCH' }
			).catch(rollbackError =>
				console.error('Could not pause backend session after device resume failed.', rollbackError)
			);
			throw error;
		}
	}
	renderSession(updatedSession);
}

async function requestDeviceService(action, deviceId, sessionType) {
	const query = new URLSearchParams({
		device_id: deviceId,
		session_type: sessionType
	});
	const response = await fetch(`${DEVICE_API_BASE_URL}/api/service/${action}?${query}`, {
		method: 'POST',
		cache: 'no-store'
	});
	if (!response.ok) {
		let detail = `Device ${action} request failed (${response.status})`;
		try {
			const result = await response.json();
			if (result.detail) detail = result.detail;
		} catch (error) {
			console.error(error);
		}
		throw new Error(detail);
	}
	return response.json();
}

async function startService(sessionType) {
	if (serviceActionPending) return;
	serviceActionPending = true;
	updateServiceButtons();
	actionStatusElement.textContent = '';

	let session = null;
	let deviceMayHaveStarted = false;
	try {
		await registerDevice();
		await fetchPaymentStatus();
		if (activeSession) {
			throw new Error('End the active session before starting another one.');
		}
		if (creditBalance <= 0) {
			throw new Error('Pay first; a positive credit balance is required.');
		}
		const creditsToSpend = creditBalance;

		const deviceId = getDeviceId();
		const response = await fetch(`${API_BASE_URL}/sessions/start`, {
			method: 'POST',
			headers: { 'Content-Type': 'application/json' },
			body: JSON.stringify({
				device_id: deviceId,
				session_type: sessionType,
				credits: creditsToSpend
			})
		});
		if (!response.ok) {
			let detail = `Could not start ${sessionType} session (${response.status})`;
			try {
				const result = await response.json();
				if (result.detail) detail = result.detail;
			} catch (error) {
				console.error(error);
			}
			throw new Error(detail);
		}
		session = await response.json();
		const durationSeconds = Math.floor(
			(Date.parse(session.expires_at) - Date.now()) / 1000
		);
		if (!Number.isSafeInteger(session.id) ||
			!Number.isSafeInteger(durationSeconds) || durationSeconds <= 0) {
			throw new Error('Backend returned an invalid session duration.');
		}

		const query = new URLSearchParams({
			device_id: deviceId,
			session_type: sessionType,
			credits: String(creditsToSpend),
			duration_seconds: String(durationSeconds)
		});
		deviceMayHaveStarted = true;
		const deviceResponse = await fetch(
			`${DEVICE_API_BASE_URL}/api/service/start?${query}`,
			{ method: 'POST', cache: 'no-store' }
		);
		if (!deviceResponse.ok) {
			deviceMayHaveStarted = deviceResponse.status >= 500;
			let detail = `Device could not start the ${sessionType} service (${deviceResponse.status})`;
			try {
				const result = await deviceResponse.json();
				if (result.detail) detail = result.detail;
			} catch (error) {
				console.error(error);
			}
			throw new Error(detail);
		}

		renderSession(session);
		actionStatusElement.textContent = sessionType === 'charging'
			? 'Charging session started.'
			: 'Wi-Fi session started. Internet forwarding is not enabled in this firmware build.';
		await fetchPaymentStatus().catch(renderPaymentUnavailable);
	} catch (error) {
		console.error(error);
		if (session) {
			if (deviceMayHaveStarted) {
				try {
					await requestDeviceService('end', getDeviceId(), sessionType);
				} catch (endError) {
					console.error('Could not stop a partially started device session.', endError);
				}
			}
			try {
				const deviceId = encodeURIComponent(getDeviceId());
				const endResponse = await fetch(
					`${API_BASE_URL}/sessions/${session.id}/end?device_id=${deviceId}`,
					{ method: 'PATCH' }
				);
				if (!endResponse.ok) {
					throw new Error(`Session cleanup failed (${endResponse.status})`);
				}
			} catch (endError) {
				console.error('Could not close the unstarted backend session.', endError);
			}
		}
		actionStatusElement.textContent = error.message;
		await fetchPaymentStatus().catch(renderPaymentUnavailable);
	} finally {
		serviceActionPending = false;
		updateServiceButtons();
	}
}

async function initializePage() {
	if (new URLSearchParams(window.location.search).get('demo') === 'active') {
		document.querySelector('.slot-status h3').textContent = 'Station Active';
		document.querySelector('.coin-status').classList.add('is-active');
		const demoStart = Date.now();
		renderSession({
			session_active: true,
			paused_at: null,
			session_start: new Date(demoStart).toISOString(),
			expires_at: new Date(demoStart + 30 * 60 * 1000).toISOString(),
			duration_seconds: 30 * 60
		});
		renderPaymentUnavailable(null);
		return;
	}

	await fetchPaymentStatus().catch(renderPaymentUnavailable);
	paymentPollTimer = setInterval(
		() => fetchPaymentStatus().catch(renderPaymentUnavailable),
		PAYMENT_POLL_INTERVAL_MS
	);

	const results = await Promise.allSettled([
		registerDevice(),
		fetchRates(),
		fetchActiveSession()
	]);
	results.forEach(result => {
		if (result.status === 'rejected') {
			console.error(result.reason);
		}
	});
}

payButton.addEventListener('click', openPaymentWindow);
connectButton.addEventListener('click', () => startService('wifi'));
chargeButton.addEventListener('click', () => startService('charging'));
pauseButton.addEventListener('click', () => {
	toggleSessionPause().catch(error => {
		console.error(error);
		actionStatusElement.textContent = error.message;
	});
});

window.addEventListener('beforeunload', () => {
	clearInterval(countdownTimer);
	clearInterval(paymentPollTimer);
});

initializePage().catch(error => {
	console.error(error);
	const status = document.querySelector('.coin-status');
	if (status) {
		status.textContent = 'Station unavailable';
	}
});
