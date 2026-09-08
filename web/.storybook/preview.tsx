import type { Preview } from "@storybook/react";
import "../src/shared/styles/theme.css";

const preview: Preview = {
	parameters: {
		controls: { matchers: { color: /(background|color)$/i, date: /Date$/i } },
		backgrounds: { disable: true },
	},
	globalTypes: {
		theme: {
			description: "Theme",
			defaultValue: "dark",
			toolbar: {
				title: "Theme",
				icon: "circlehollow",
				items: ["dark", "light"],
				dynamicTitle: true,
			},
		},
	},
	decorators: [
		(Story, context) => {
			const theme = context.globals.theme ?? "dark";
			document.documentElement.setAttribute("data-theme", theme);
			document.body.style.background = "var(--color-canvas)";
			document.body.style.color = "var(--color-ink)";
			document.body.style.padding = "1.5rem";
			return <Story />;
		},
	],
};

export default preview;
