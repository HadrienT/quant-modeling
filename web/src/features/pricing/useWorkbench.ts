import { useCallback, useMemo } from "react";
import { useNavigate, useSearch } from "@tanstack/react-router";
import { decodeParams, encodeParams } from "@/shared/products/workbenchLink";
import type { ComputeDevice } from "@/shared/api";
import {
	CATALOG_BY_KEY,
	DEFAULT_PRODUCT_KEY,
	type EngineKey,
} from "@/shared/products";

/**
 * All view state lives in the URL (WP 07 §6): a pricing session is a link.
 * `p` is a base64url-encoded JSON of the form values. Kept under ~120 lines.
 */

export type WorkbenchState = {
	productKey: string;
	engine: EngineKey;
	values: Record<string, unknown>;
	compare: Record<string, unknown> | null;
};

const encode = encodeParams;
const decode = decodeParams;

export function useWorkbench() {
	const search = useSearch({ from: "/price" });
	const navigate = useNavigate({ from: "/price" });

	const productKey =
		search.product && CATALOG_BY_KEY.has(search.product)
			? search.product
			: DEFAULT_PRODUCT_KEY;
	const descriptor = CATALOG_BY_KEY.get(productKey)!;

	const engine = (search.engine ?? descriptor.engines[0]!.key) as EngineKey;
	const values = useMemo(
		() => ({ ...descriptor.defaults, ...decode(search.p, {}) }),
		[descriptor, search.p],
	);
	const compare = decode<Record<string, unknown> | null>(search.compare, null);
	const device: ComputeDevice = search.device ?? "cpu";

	const setProduct = useCallback(
		(key: string) => {
			navigate({
				search: () => ({ product: key, engine: undefined, p: undefined }),
			});
		},
		[navigate],
	);

	const setEngine = useCallback(
		(e: EngineKey) => {
			navigate({ search: (prev) => ({ ...prev, engine: e }) });
		},
		[navigate],
	);

	const setValues = useCallback(
		(next: Record<string, unknown>) => {
			navigate({ search: (prev) => ({ ...prev, p: encode(next) }) });
		},
		[navigate],
	);

	// Only Monte-Carlo runs on the GPU: asking for it switches the engine.
	const setDevice = useCallback(
		(d: ComputeDevice) => {
			navigate({
				search: (prev) => ({
					...prev,
					device: d === "cpu" ? undefined : d,
					engine: d === "cpu" ? prev.engine : "mc",
				}),
			});
		},
		[navigate],
	);

	const setCompare = useCallback(
		(next: Record<string, unknown> | null) => {
			navigate({
				search: (prev) => ({
					...prev,
					compare: next ? encode(next) : undefined,
				}),
			});
		},
		[navigate],
	);

	return {
		descriptor,
		productKey,
		engine,
		values,
		compare,
		device,
		setDevice,
		setProduct,
		setEngine,
		setValues,
		setCompare,
	};
}

const PRESETS_KEY = "qm-pricing-presets";
export type Preset = {
	name: string;
	product: string;
	engine: string;
	values: Record<string, unknown>;
};

export function loadPresets(): Preset[] {
	try {
		return JSON.parse(localStorage.getItem(PRESETS_KEY) ?? "[]");
	} catch {
		return [];
	}
}
export function savePreset(p: Preset) {
	const all = loadPresets().filter((x) => x.name !== p.name);
	all.push(p);
	try {
		localStorage.setItem(PRESETS_KEY, JSON.stringify(all));
	} catch {
		/* private mode */
	}
}
export function deletePreset(name: string) {
	try {
		localStorage.setItem(
			PRESETS_KEY,
			JSON.stringify(loadPresets().filter((x) => x.name !== name)),
		);
	} catch {
		/* ignore */
	}
}
