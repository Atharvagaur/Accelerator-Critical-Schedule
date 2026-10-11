import csv
import statistics
import sys

def calculate_correlation(x, y):
    if len(x) != len(y) or len(x) < 2:
        return None

    mu_x = statistics.mean(x)
    mu_y = statistics.mean(y)

    num = sum((xi - mu_x) * (yi - mu_y) for xi, yi in zip(x, y))
    den = (sum((xi - mu_x)**2 for xi in x) * sum((yi - mu_y)**2 for yi in y))**0.5

    return num / den if den != 0 else 0

def analyze_metrics(file_path, baseline_path=None):
    try:
        # Helper to load and filter CSV
        def load_csv(path):
            try:
                with open(path, 'r') as f:
                    reader = csv.DictReader(f)
                    data = list(reader)
                return data[2:] if len(data) > 2 else []
            except FileNotFoundError:
                return []

        main_data = load_csv(file_path)
        if not main_data:
            print("Error: main CSV file is empty or too short.")
            return

        # Define the metrics we care about
        metrics_config = {
            'gpu_idle_fraction': 'GPU Idle (%)',
            'step_time_us': 'Step Time (us)',
            'iter_latency_us': 'Iteration Latency (us)',
            'runnable_tasks': 'Runnable Tasks'
        }

        print("="*80)
        print(" PRELIMINARY RESULTS: ACCELERATOR-CRITICAL SCHEDULER")
        print("="*80)
        print(f"Samples analyzed: {len(main_data)}")
        print("-"*80)
        print(f"{'Metric':<25} | {'Mean':<10} | {'p50':<10} | {'p95':<10} | {'Max':<10}")
        print("-"*80)

        results_map = {}
        for col, label in metrics_config.items():
            try:
                values = [float(row[col]) for row in main_data if row[col]]
                if not values:
                    print(f"{label:<25} | {'N/A':>10} | {'N/A':>10} | {'N/A':>10} | {'N/A':>10}")
                    continue

                mean_val = statistics.mean(values)
                p50 = statistics.median(values)

                sorted_vals = sorted(values)
                p95_idx = int(len(sorted_vals) * 0.95)
                p95 = sorted_vals[min(p95_idx, len(sorted_vals)-1)]

                max_val = max(values)
                results_map[col] = values

                if col == 'gpu_idle_fraction':
                    print(f"{label:<25} | {mean_val/10:>9.2f}% | {p50/10:>9.2f}% | {p95/10:>9.2f}% | {max_val/10:>9.2f}%")
                else:
                    print(f"{label:<25} | {mean_val:>10.1f} | {p50:>10.1f} | {p95:>10.1f} | {max_val:>10.1f}")
            except KeyError:
                print(f"{label:<25} | {'N/A':>10} | {'N/A':>10} | {'N/A':>10} | {'N/A':>10}")

        print("="*80)

        # Baseline Comparison
        if baseline_path:
            print("\nComparing against Baseline...")
            base_data = load_csv(baseline_path)
            if base_data:
                for col, label in metrics_config.items():
                    base_vals = [float(row[col]) for row in base_data if row[col]]
                    if base_vals and col in results_map:
                        # Calculate Means
                        main_mean = statistics.mean(results_map[col])
                        base_mean = statistics.mean(base_vals)
                        diff_mean = main_mean - base_mean

                        # Calculate p50 (Median)
                        main_p50 = statistics.median(results_map[col])
                        base_p50 = statistics.median(base_vals)
                        diff_p50 = main_p50 - base_p50

                        # Calculate p95
                        def get_p95(vals):
                            s = sorted(vals)
                            return s[int(len(s) * 0.95)] if s else 0

                        main_p95 = get_p95(results_map[col])
                        base_p95 = get_p95(base_vals)
                        diff_p95 = main_p95 - base_p95

                        if col == 'gpu_idle_fraction':
                            print(f"{label:<25} | Mean Delta: {diff_mean/10:>8.2f}% | p50 Delta: {diff_p50/10:>8.2f}% | p95 Delta: {diff_p95/10:>8.2f}%")
                        else:
                            print(f"{label:<25} | Mean Delta: {diff_mean:>8.1f} | p50 Delta: {diff_p50:>8.1f} | p95 Delta: {diff_p95:>8.1f}")
            else:
                print("Baseline file not found or empty.")

        # Correlation Analysis
        if 'gpu_idle_fraction' in results_map and 'runnable_tasks' in results_map:
            x = results_map['runnable_tasks']
            y = results_map['gpu_idle_fraction']
            corr = calculate_correlation(x, y)
            print(f"\nCorrelation (Runnable vs GPU Idle): {corr:.4f}")
            if corr is not None:
                if corr > 0.5: print("Result: Strong positive correlation (Expected)")
                elif corr < -0.5: print("Result: Strong negative correlation")
                else: print("Result: Weak or no correlation")

        if 'gpu_idle_fraction' in results_map and 'step_time_us' in results_map:
            x = results_map['step_time_us']
            y = results_map['gpu_idle_fraction']
            corr = calculate_correlation(x, y)
            print(f"Correlation (Step Time vs GPU Idle): {corr:.4f}")

    except Exception as e:
        print(f"Analysis failed: {e}")

if __name__ == "__main__":
    main_csv = "metrics.csv"
    baseline_csv = sys.argv[1] if len(sys.argv) > 1 else None
    analyze_metrics(main_csv, baseline_csv)

