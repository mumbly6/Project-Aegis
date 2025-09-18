# Step 1: Import libraries
import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt
from sklearn.datasets import load_iris

# Step 2: Load the Iris dataset from sklearn
iris = load_iris()

# Step 3: Convert to a pandas DataFrame
df = pd.DataFrame(iris.data, columns=iris.feature_names)
df['species'] = pd.Categorical.from_codes(iris.target, iris.target_names)

# Step 4: Inspect the first 5 rows
print("First 5 rows of dataset:")
print(df.head())

# Step 5: Check data types and missing values
print("\nData info:")
print(df.info())
print("\nMissing values:")
print(df.isnull().sum())

# Step 6: Cleaning (Iris dataset has no missing values, but if it did we’d handle them)
df = df.dropna()  # drop missing values (or df.fillna(value) to fill)

# Step 1: Summary statistics
print("\nSummary statistics:")
print(df.describe())

# Step 2: Group by species and compute mean
print("\nAverage measurements per species:")
print(df.groupby('species').mean())

# Step 3: Identify patterns
# Example: which species has longest petals?
petal_means = df.groupby('species')['petal length (cm)'].mean()
print("\nMean petal lengths by species:")
print(petal_means)

# Step 1: Line Chart (not time series, but let’s plot average petal length per species as a line)
plt.figure(figsize=(6,4))
petal_means.plot(kind='line', marker='o')
plt.title("Average Petal Length per Species")
plt.xlabel("Species")
plt.ylabel("Petal Length (cm)")
plt.grid(True)
plt.show()

# Step 2: Bar Chart (average sepal width per species)
plt.figure(figsize=(6,4))
df.groupby('species')['sepal width (cm)'].mean().plot(kind='bar', color=['red','green','blue'])
plt.title("Average Sepal Width per Species")
plt.xlabel("Species")
plt.ylabel("Sepal Width (cm)")
plt.show()

# Step 3: Histogram (distribution of petal length)
plt.figure(figsize=(6,4))
plt.hist(df['petal length (cm)'], bins=20, color='purple', alpha=0.7)
plt.title("Distribution of Petal Lengths")
plt.xlabel("Petal Length (cm)")
plt.ylabel("Frequency")
plt.show()

# Step 4: Scatter Plot (sepal length vs petal length)
plt.figure(figsize=(6,4))
sns.scatterplot(x="sepal length (cm)", y="petal length (cm)", hue="species", data=df, palette="deep")
plt.title("Sepal Length vs Petal Length")
plt.xlabel("Sepal Length (cm)")
plt.ylabel("Petal Length (cm)")
plt.legend(title="Species")
plt.show()
# Step 5: Box Plot (sepal width by species)
plt.figure(figsize=(6,4))       
sns.boxplot(x="species", y="sepal width (cm)", data=df, palette="deep")
plt.title("Sepal Width by Species")
plt.xlabel("Species")
plt.ylabel("Sepal Width (cm)")
plt.show()              