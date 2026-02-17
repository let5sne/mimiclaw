# 文档回归样本说明

- `notes.txt`：基础文本样本
- `food.csv`：基础表格文本样本
- `food.xlsx`：Office OpenXML 表格样本（可直接用于 `--office`）
- `food_legacy.xls`：旧版 Excel 二进制样本（可选，若缺失会在回归中自动跳过）

建议把真实业务样本（脱敏后）追加到同目录，并扩展 `tools/doc_regression_manifest.*.json`。
