import { Vector2 } from "./krol";

describe("Krol", () => {
  describe("Vector2", () => {
    it("should create a vector2", () => {
      const vector2 = new Vector2(1, 2);
      expect(vector2.x).toBe(1);
      expect(vector2.y).toBe(2);
    });

    it("should add two vectors", () => {
      const vector1 = new Vector2(1, 2);
      const vector2 = new Vector2(3, 4);
      const result = vector1.add(vector2);
      expect(result.x).toBe(4);
      expect(result.y).toBe(6);
    });

    it("should subtract two vectors", () => {
      const vector1 = new Vector2(1, 2);
      const vector2 = new Vector2(3, 4);
      const result = vector1.sub(vector2);
      expect(result.x).toBe(-2);
      expect(result.y).toBe(-2);
    });

    it("should multiply two vectors", () => {
      const vector1 = new Vector2(1, 2);
      const vector2 = new Vector2(3, 4);
      const result = vector1.multiply(vector2);
      expect(result.x).toBe(3);
      expect(result.y).toBe(8);
    });

    it("should divide two vectors", () => {
      const vector1 = new Vector2(1, 2);
      const vector2 = new Vector2(3, 4);
      const result = vector1.divide(vector2);
      expect(result.x).toBe(1 / 3);
      expect(result.y).toBe(2 / 4);
    });

    it("should invert a vector", () => {
      const vector = new Vector2(1, 2);
      const result = vector.invert();
      expect(result.x).toBe(-1);
      expect(result.y).toBe(-2);
    });

    it("should get the length of a vector", () => {
      const vector = new Vector2(3, 4);
      const result = vector.magnitude();
      expect(result).toBe(5);
    });

    it("should normalize a vector", () => {
      const vector = new Vector2(1, 1);
      const result = vector.normalize();

      expect(result.x).toBe(1 / Math.sqrt(2));
      expect(result.y).toBe(1 / Math.sqrt(2));
    });

    it("should normalize a vector with a length of 0", () => {
      const vector = new Vector2(0, 0);
      const result = vector.normalize();

      expect(result.x).toBe(0);
      expect(result.y).toBe(0);
    });
  });
});
