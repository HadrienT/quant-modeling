import { useEffect } from "react";
import { useNavigate, useSearch } from "@tanstack/react-router";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import type { Portfolio } from "@/shared/api";
import type { PortfolioRepository } from "@/shared/portfolio";
import { toast } from "@/shared/ui";

/**
 * The list of portfolios and what can be done to it: select (kept in the
 * URL), create, rename, delete, import — and the read-only demos, opened
 * with `?demo=` and copied into one's own portfolios. Oldest first, so a portfolio does
 * not move in the list when it is edited.
 */
export function usePortfolioBook(repo: PortfolioRepository) {
	const qc = useQueryClient();
	const search = useSearch({ from: "/portfolio" });
	const navigate = useNavigate({ from: "/portfolio" });
	const list = useQuery({
		queryKey: ["portfolio", "list", repo.kind],
		queryFn: async () =>
			(await repo.list()).sort((a, b) =>
				(a.created_at ?? "").localeCompare(b.created_at ?? ""),
			),
	});
	const portfolios = list.data ?? [];
	const known = portfolios.some((p) => p.id === search.id);
	const selectedId = known ? search.id! : (portfolios[0]?.id ?? null);
	const demoId = search.demo ?? null;

	useEffect(() => {
		if (!demoId && list.data && selectedId !== (search.id ?? null)) {
			navigate({ search: () => (selectedId ? { id: selectedId } : {}) });
		}
	}, [demoId, list.data, selectedId, search.id, navigate]);

	const select = (id: string) => navigate({ search: () => ({ id }) });
	const selectDemo = (id: string) => navigate({ search: () => ({ demo: id }) });
	const refresh = () => list.refetch();

	async function create() {
		const created = await repo.create(`Portfolio ${portfolios.length + 1}`);
		await refresh();
		select(created.id);
	}

	async function rename(id: string, name: string) {
		try {
			await repo.rename(id, name);
			await refresh();
			void qc.invalidateQueries({
				queryKey: ["portfolio", "detail", repo.kind, id],
			});
		} catch {
			toast.error("Could not rename the portfolio");
		}
	}

	async function remove(id: string) {
		try {
			await repo.remove(id);
			qc.removeQueries({ queryKey: ["portfolio", "detail", repo.kind, id] });
			const rest = portfolios.filter((p) => p.id !== id);
			await refresh();
			if (id === selectedId && rest[0]) select(rest[0].id);
		} catch {
			toast.error("Could not delete the portfolio");
		}
	}

	async function importJson(file: File) {
		try {
			const parsed = JSON.parse(await file.text()) as Portfolio;
			const created = await repo.importPortfolio(parsed);
			await refresh();
			select(created.id);
			toast.success(`Imported "${created.name}"`);
		} catch {
			toast.error("Not a valid portfolio JSON file");
		}
	}

	/** A demo becomes one's own portfolio: same trades, now editable. */
	async function copyDemo(demo: Portfolio) {
		try {
			const created = await repo.importPortfolio({ ...demo, owner: "" });
			await refresh();
			select(created.id);
			toast.success(`"${demo.name}" copied to your portfolios`);
		} catch {
			toast.error("Could not copy the demo");
		}
	}

	return {
		list,
		portfolios,
		selectedId,
		demoId,
		select,
		selectDemo,
		create,
		rename,
		remove,
		importJson,
		copyDemo,
	};
}
