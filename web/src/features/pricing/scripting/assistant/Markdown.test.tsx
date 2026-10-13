import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";
import { Markdown } from "./Markdown";

describe("Markdown", () => {
	it("renders emphasis instead of showing the asterisks", () => {
		render(<Markdown text={"The **strike** is *fixed* at `100`."} />);
		expect(screen.getByText("strike").tagName).toBe("STRONG");
		expect(screen.getByText("fixed").tagName).toBe("EM");
		expect(screen.getByText("100").tagName).toBe("CODE");
		expect(screen.queryByText(/\*/)).toBeNull();
	});

	it("renders lists, headings and tables", () => {
		render(
			<Markdown
				text={
					"## Assumptions\n\n- monthly fixings\n- strike 100\n\n" +
					"| Variable | Role |\n|---|---|\n| acc | running sum |"
				}
			/>,
		);
		expect(
			screen.getByRole("heading", { name: "Assumptions" }),
		).toBeInTheDocument();
		expect(screen.getAllByRole("listitem")).toHaveLength(2);
		expect(screen.getByRole("table")).toBeInTheDocument();
		expect(
			screen.getByRole("columnheader", { name: "Role" }),
		).toBeInTheDocument();
	});

	it("typesets LaTeX, inline and display, in every delimiter style", async () => {
		render(
			<Markdown
				text={
					"The payoff is $\\max(S_T - K, 0)$, with \\(K\\) the strike:\n" +
					"\\[ A = \\frac{1}{n} \\sum_i S_i \\]"
				}
			/>,
		);
		// KaTeX keeps the source in an annotation next to what it draws.
		const sources = await screen.findAllByText(
			(_, el) => el?.tagName.toLowerCase() === "annotation",
		);
		expect(sources.map((el) => el.textContent)).toEqual([
			"\\max(S_T - K, 0)",
			"K",
			"A = \\frac{1}{n} \\sum_i S_i",
		]);
		expect(screen.queryByText(/[$]|\\\[|\\\(/)).toBeNull();
	});

	it("keeps prices in dollars as text", () => {
		render(<Markdown text={"Strike $100, barrier $130."} />);
		expect(screen.getByText("Strike $100, barrier $130.")).toBeInTheDocument();
	});

	it("opens links in a new tab and does not interpret raw HTML", () => {
		render(
			<Markdown
				text={'See [the docs](https://example.com). <b id="x">raw</b>'}
			/>,
		);
		const link = screen.getByRole("link", { name: "the docs" });
		expect(link).toHaveAttribute("target", "_blank");
		expect(link).toHaveAttribute("rel", "noreferrer noopener");
		expect(screen.getByText(/<b id="x">raw<\/b>/)).toBeInTheDocument();
	});
});
