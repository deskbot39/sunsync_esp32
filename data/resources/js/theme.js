const themeToggle = document.querySelector('.theme-toggle');
const savedTheme = localStorage.getItem('sun-sync-theme');

function applyTheme(theme) {
    document.documentElement.dataset.theme = theme;
    themeToggle.textContent = theme === 'light' ? 'Dark theme' : 'Light theme';
    themeToggle.setAttribute('aria-label', `Switch to ${theme === 'light' ? 'dark' : 'light'} theme`);
}

applyTheme(savedTheme || 'dark');

themeToggle.addEventListener('click', () => {
    const nextTheme = document.documentElement.dataset.theme === 'light' ? 'dark' : 'light';
    localStorage.setItem('sun-sync-theme', nextTheme);
    applyTheme(nextTheme);
});
