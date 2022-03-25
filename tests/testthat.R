library(testthat)
library(processx)

if (Sys.getenv("NOT_CRAN") == "true") {
  test_check("processx", reporter = "summary")
}
