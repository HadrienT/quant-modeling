import js from "@eslint/js";
import globals from "globals";
import tseslint from "typescript-eslint";
import reactHooks from "eslint-plugin-react-hooks";
import reactRefresh from "eslint-plugin-react-refresh";
import importPlugin from "eslint-plugin-import";
import jsxA11y from "eslint-plugin-jsx-a11y";
import testingLibrary from "eslint-plugin-testing-library";

const NEW_CODE = ["src/app/**", "src/features/**", "src/shared/**"];

export default tseslint.config(
	{
		ignores: [
			"dist",
			"storybook-static",
			"node_modules",
			"src/shared/api/schema.gen.ts",
			"src/routeTree.gen.ts",
			"playwright-report",
			"coverage",
		],
	},
	js.configs.recommended,
	...tseslint.configs.recommended,
	{
		files: ["**/*.{ts,tsx}"],
		languageOptions: {
			ecmaVersion: 2023,
			globals: { ...globals.browser, ...globals.node },
		},
		plugins: {
			"react-hooks": reactHooks,
			"react-refresh": reactRefresh,
			import: importPlugin,
			"jsx-a11y": jsxA11y,
		},
		settings: {
			"import/resolver": {
				typescript: { project: "./tsconfig.json" },
				node: true,
			},
		},
		rules: {
			...reactHooks.configs.recommended.rules,
			...jsxA11y.flatConfigs.recommended.rules,
			"react-refresh/only-export-components": [
				"warn",
				{ allowConstantExport: true },
			],
			"@typescript-eslint/no-unused-vars": [
				"error",
				{ argsIgnorePattern: "^_", varsIgnorePattern: "^_" },
			],
			"@typescript-eslint/consistent-type-imports": [
				"error",
				{ prefer: "type-imports", fixStyle: "inline-type-imports" },
			],
			"import/order": [
				"warn",
				{
					groups: [
						"builtin",
						"external",
						"internal",
						"parent",
						"sibling",
						"index",
					],
					"newlines-between": "never",
				},
			],
			"no-console": ["error", { allow: ["warn", "error"] }],
		},
	},

	/* ── Architecture discipline — new code only (WP 12 §3) ──────────────── */
	{
		files: NEW_CODE,
		rules: {
			"import/no-restricted-paths": [
				"error",
				{
					zones: [
						{
							target: "src/shared",
							from: ["src/features", "src/app"],
							message:
								"shared/ must not import features/ or app/ (dependencies.md §5).",
						},
						{
							target: "src/features/pricing",
							from: ["src/features"],
							except: ["src/features/pricing"],
							message: "A feature must not import another feature.",
						},
						{
							target: "src/features/market",
							from: ["src/features"],
							except: ["src/features/market"],
							message: "A feature must not import another feature.",
						},
						{
							target: "src/features/portfolio",
							from: ["src/features"],
							except: ["src/features/portfolio"],
							message: "A feature must not import another feature.",
						},
						{
							target: "src/features/strategies",
							from: ["src/features"],
							except: ["src/features/strategies"],
							message: "A feature must not import another feature.",
						},
						{
							target: "src/features/backtest",
							from: ["src/features"],
							except: ["src/features/backtest"],
							message: "A feature must not import another feature.",
						},
					],
				},
			],
		},
	},

	/* Tests */
	{
		files: ["**/*.{test,spec}.{ts,tsx}", "tests/**/*.{ts,tsx}"],
		plugins: { "testing-library": testingLibrary },
		rules: {
			...testingLibrary.configs.react.rules,
		},
	},

	/* Legacy front — deleted WP by WP; keep it merely lint-clean, not strict */
	{
		files: [
			"src/App.tsx",
			"src/main.tsx",
			"src/api/**",
			"src/auth/**",
			"src/components/**",
			"src/pages/**",
			"src/utils/**",
			"src/vite-env.d.ts",
		],
		rules: {
			"import/order": "off",
			"jsx-a11y/no-autofocus": "off",
			"jsx-a11y/click-events-have-key-events": "off",
			"jsx-a11y/no-static-element-interactions": "off",
			"jsx-a11y/label-has-associated-control": "off",
			"@typescript-eslint/consistent-type-imports": "off",
			"@typescript-eslint/no-explicit-any": "off",
			"@typescript-eslint/no-unused-vars": "off",
			"@typescript-eslint/ban-ts-comment": "off",
			"react-hooks/exhaustive-deps": "off",
			"react-refresh/only-export-components": "off",
		},
	},

	/* Design-system dir: components legitimately co-export variants & types */
	{
		files: ["src/shared/ui/**"],
		rules: {
			"react-refresh/only-export-components": "off",
			"jsx-a11y/label-has-associated-control": "off",
		},
	},

	/* App composition modules export routers / stores next to components */
	{
		files: [
			"src/app/router.tsx",
			"src/app/theme.ts",
			"src/app/**/index.ts",
			"src/**/*.stories.tsx",
			"src/shared/viz/surface/SurfaceScene.tsx",
		],
		rules: { "react-refresh/only-export-components": "off" },
	},

	/* Config files & tooling (Node scripts) */
	{
		files: [
			"*.config.{ts,js}",
			".storybook/**",
			"e2e/**",
			"scripts/**",
			"tests/**",
		],
		languageOptions: { globals: { ...globals.node } },
		rules: { "no-console": "off", "import/order": "off", "no-undef": "off" },
	},
);
