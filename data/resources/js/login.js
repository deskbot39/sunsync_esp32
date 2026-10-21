const API_BASE_URL = (typeof window.SUNSYNC_API_BASE === 'string'
    ? window.SUNSYNC_API_BASE
    : 'http://localhost:8000').replace(/\/$/, '');
const TOKEN_KEY = 'sunSyncAccessToken';
const USERNAME_KEY = 'sunSyncRememberedUsername';

const form = document.querySelector('#form-login');
const usernameInput = document.querySelector('#username');
const passwordInput = document.querySelector('#password');
const rememberDevice = document.querySelector('#remember-device');
const togglePassword = document.querySelector('#toggle-password');
const submitButton = document.querySelector('#login-submit');
const errorMessage = document.querySelector('.login-error');

const rememberedUsername = localStorage.getItem(USERNAME_KEY);
if (rememberedUsername) {
	usernameInput.value = rememberedUsername;
	rememberDevice.checked = true;
}

function setLoginMessage(message, type = 'error') {
	errorMessage.textContent = message || '';
	errorMessage.classList.remove('error', 'success');
	if (message) {
		errorMessage.classList.add(type);
	}
}

function validateLoginInputs() {
	const username = usernameInput.value.trim();
	const password = passwordInput.value;

	if (!username) {
		setLoginMessage('Please enter your username.');
		usernameInput.focus();
		return false;
	}

	if (username.length < 3) {
		setLoginMessage('Username must be at least 3 characters long.');
		usernameInput.focus();
		return false;
	}

	if (!password) {
		setLoginMessage('Please enter your password.');
		passwordInput.focus();
		return false;
	}

	if (password.length < 6) {
		setLoginMessage('Password must be at least 6 characters long.');
		passwordInput.focus();
		return false;
	}

	setLoginMessage('', 'error');
	return true;
}

togglePassword.addEventListener('click', () => {
	const isPassword = passwordInput.type === 'password';
	passwordInput.type = isPassword ? 'text' : 'password';
	togglePassword.textContent = isPassword ? 'Hide' : 'Show';
	togglePassword.setAttribute('aria-label', `${isPassword ? 'Hide' : 'Show'} password`);
});

usernameInput.addEventListener('input', () => {
	if (errorMessage.textContent) {
		setLoginMessage('', 'error');
	}
});

passwordInput.addEventListener('input', () => {
	if (errorMessage.textContent) {
		setLoginMessage('', 'error');
	}
});

form.addEventListener('submit', async event => {
	event.preventDefault();
	if (!validateLoginInputs()) {
		return;
	}

	submitButton.disabled = true;
	submitButton.textContent = 'Signing In...';
	setLoginMessage('', 'error');

	try {
		const username = usernameInput.value.trim();
		const password = passwordInput.value;
		const body = new URLSearchParams({ username, password });
		const response = await fetch(`${API_BASE_URL}/auth/login`, {
			method: 'POST',
			headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
			body,
		});

		if (!response.ok) {
			const error = await response.json().catch(() => ({}));
			const detail = error.detail || 'Incorrect username or password.';
			throw new Error(detail);
		}

		const { access_token: accessToken } = await response.json();
		const storage = rememberDevice.checked ? localStorage : sessionStorage;
		const otherStorage = rememberDevice.checked ? sessionStorage : localStorage;
		otherStorage.removeItem(TOKEN_KEY);
		storage.setItem(TOKEN_KEY, accessToken);
		if (rememberDevice.checked) {
			localStorage.setItem(USERNAME_KEY, username);
		} else {
			localStorage.removeItem(USERNAME_KEY);
		}

		setLoginMessage('Sign in successful. Redirecting to dashboard...', 'success');
		submitButton.disabled = true;
		submitButton.textContent = 'Redirecting...';
		window.setTimeout(() => {
			window.location.href = './dashboard.html';
		}, 900);
	} catch (error) {
		setLoginMessage(error.message || 'Unable to sign in. Please check your credentials and try again.');
		submitButton.disabled = false;
		submitButton.textContent = 'Sign In to Dashboard';
	}
});