import { useMe } from "@/shared/session";
import {
	type PortfolioRepository,
	localRepository,
	serverRepository,
} from "@/shared/portfolio";

/**
 * Returns the active repository. Anonymous is the DEFAULT path (WP 04 §5), not a
 * degraded mode — most visitors never sign in. A server portfolio needs a
 * session; without one we fall back to local rather than blocking.
 */
export function useRepository(): PortfolioRepository {
	const me = useMe();
	return me.data?.username ? serverRepository : localRepository;
}
