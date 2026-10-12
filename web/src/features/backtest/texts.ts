/**
 * The notions of the page, each with the reference it comes from. The
 * figures carry their own convention and method, worded by the engine; these
 * texts say why the page shows them the way it does.
 */
export const SOURCES = {
	lo: "Lo, A. (2002). The statistics of Sharpe ratios. Financial Analysts Journal 58(4).",
	bootstrap:
		"Politis, D. & Romano, J. (1994). The stationary bootstrap. Journal of the American Statistical Association 89(428); block length: Politis & White (2004), corrected by Patton, Politis & White (2009), Econometric Reviews.",
	memmel:
		"Jobson, J. & Korkie, B. (1981). Performance hypothesis testing with the Sharpe and Treynor measures. Journal of Finance 36(4); Memmel, C. (2003). Performance hypothesis testing with the Sharpe ratio. Finance Letters 1.",
	naive:
		"DeMiguel, V., Garlappi, L. & Uppal, R. (2009). Optimal versus naive diversification: how inefficient is the 1/N portfolio strategy? Review of Financial Studies 22(5).",
	psr: "Bailey, D. & López de Prado, M. (2012). The Sharpe ratio efficient frontier. Journal of Risk 15(2).",
	factors:
		"Fama, E. & French, K. (1993). Common risk factors in the returns on stocks and bonds. Journal of Financial Economics 33(1); (2015). A five-factor asset pricing model. Journal of Financial Economics 116(1). Standard errors: Newey, W. & West, K. (1987). Econometrica 55(3).",
	deflated:
		"Bailey, D. & López de Prado, M. (2014). The deflated Sharpe ratio: correcting for selection bias, backtest overfitting and non-normality. Journal of Portfolio Management 40(5).",
	overfitting:
		"Bailey, D., Borwein, J., López de Prado, M. & Zhu, Q. (2017). The probability of backtest overfitting. Journal of Computational Finance 20(4).",
	purged:
		"López de Prado, M. (2018). Advances in Financial Machine Learning, chapters 7 and 12. Wiley.",
	costs:
		"Corwin, S. & Schultz, P. (2012). A simple way to estimate bid-ask spreads from daily high and low prices. Journal of Finance 67(2); Abdi, F. & Ranaldo, A. (2017). A simple estimation of bid-ask spreads from daily close, high, and low prices. Review of Financial Studies 30(12).",
	breakEven:
		"Novy-Marx, R. & Velikov, M. (2016). A taxonomy of anomalies and their trading costs. Review of Financial Studies 29(1).",
	shrinkage:
		"Ledoit, O. & Wolf, M. (2004). A well-conditioned estimator for large-dimensional covariance matrices. Journal of Multivariate Analysis 88(2).",
} as const;

export const TEX = {
	sharpe: String.raw`\widehat{sr}=\frac{\bar r_e}{\hat\sigma_e},\qquad \mathrm{se}\big(\widehat{sr}\big)\approx\sqrt{\frac{1+\tfrac12\widehat{sr}^2-\hat\gamma_3\,\widehat{sr}+\tfrac{\hat\gamma_4-3}{4}\widehat{sr}^2}{T}},\qquad \widehat{SR}=\widehat{sr}\sqrt{P}`,
	deflated: String.raw`\mathrm{DSR}=\Phi\!\left(\frac{\big(\widehat{SR}-SR_0\big)\sqrt{T-1}}{\sqrt{1-\hat\gamma_3\widehat{SR}+\tfrac{\hat\gamma_4-1}{4}\widehat{SR}^2}}\right),\quad SR_0=\sqrt{V}\Big((1-\gamma)\,\Phi^{-1}\big(1-\tfrac1N\big)+\gamma\,\Phi^{-1}\big(1-\tfrac1{Ne}\big)\Big)`,
	overfitting: String.raw`\lambda=\ln\frac{\bar\omega}{1-\bar\omega},\qquad \mathrm{PBO}=\Pr\big[\lambda\le 0\big]`,
	factors: String.raw`r_t-r_{f,t}=\alpha+\sum_k\beta_k F_{k,t}+\varepsilon_t`,
} as const;

/** Why the default universe is made of funds. */
export const SURVIVORSHIP =
	"The database has prices for the companies that are still listed: of the 491 members of the S&P 500 in January 2000, 186 have a history. A backtest on today's stocks is run on the survivors and flatters every strategy. The funds offered first have no such gap, which is why they are the default.";

/** What the engine measured about spread estimators, kept in view. */
export const SPREAD_ESTIMATORS =
	"The cost is the one declared above, not an estimate. The estimators that infer a spread from daily highs, lows and closes return about 60 and 30 basis points on a liquid stock whose true spread is 3: they are an upper bound, useful for illiquid names, and are not used here.";
