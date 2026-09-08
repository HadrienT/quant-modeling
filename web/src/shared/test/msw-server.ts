import { setupServer } from "msw/node";
import { handlers } from "./handlers";

/** Node MSW server used by Vitest. Browser worker lives in ./browser.ts (WP 02). */
export const server = setupServer(...handlers);
