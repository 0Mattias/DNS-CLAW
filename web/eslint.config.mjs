import nextConfig from "eslint-config-next";

const eslintConfig = [
  ...nextConfig,
  {
    ignores: ["lib/__tests__/**"],
  },
];

export default eslintConfig;
